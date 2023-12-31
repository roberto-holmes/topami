#include <stdarg.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dreamberd.hpp"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"

using namespace llvm;

//===----------------------------------------------------------------------===//
// Lexer
//===----------------------------------------------------------------------===//

// The lexer returns tokens [0-255] if it is an unknown character, otherwise one
// of these for known things.
enum Token {
	tok_eof = -1,

	// commands
	tok_function = -2,
	tok_extern = -3,

	// primary
	tok_identifier = -4,
	tok_number = -5
};

static std::string IdentifierStr;  // Filled in if tok_identifier
static double NumVal;			   // Filled in if tok_number

std::string file_name;
std::ifstream file;

/// gettok - Return the next token from file.
static char gettok() {
	static char LastChar = ' ';

	// Skip any whitespace.
	while (isspace(LastChar)) file.get(LastChar);

	if (isalpha(LastChar)) {  // identifier: [a-zA-Z][a-zA-Z0-9]*
		IdentifierStr = LastChar;
		file.get(LastChar);
		while (isalnum(LastChar)) {
			IdentifierStr += LastChar;
			file.get(LastChar);
		}

		if (dreamberd::is_function_definition(IdentifierStr))
			return tok_function;
		if (IdentifierStr == "extern")
			return tok_extern;
		return tok_identifier;
	}

	if (isdigit(LastChar) || LastChar == '.') {	 // Number: [0-9.]+
		std::string NumStr;
		do {
			NumStr += LastChar;
			file.get(LastChar);
		} while (isdigit(LastChar) || LastChar == '.');

		NumVal = strtod(NumStr.c_str(), nullptr);
		return tok_number;
	}

	if (LastChar == '/' && file.peek() == '/') {
		// Comment until end of line.
		do
			file.get(LastChar);
		while (!file.eof() && LastChar != '\n' && LastChar != '\r');

		if (!file.eof())
			return gettok();
	}

	// Check for end of file.
	if (file.eof())
		return tok_eof;

	// Otherwise, just return the character as its ascii value.
	char ThisChar = LastChar;
	file.get(LastChar);
	return ThisChar;
}

//===----------------------------------------------------------------------===//
// Abstract Syntax Tree (aka Parse Tree)
//===----------------------------------------------------------------------===//

namespace {

	/// ExprAST - Base class for all expression nodes.
	class ExprAST {
	   public:
		virtual ~ExprAST() = default;

		virtual Value* codegen() = 0;
	};

	/// NumberExprAST - Expression class for numeric literals like "1.0".
	class NumberExprAST : public ExprAST {
		double Val;

	   public:
		NumberExprAST(double Val) : Val(Val) {}

		Value* codegen() override;
	};

	/// VariableExprAST - Expression class for referencing a variable, like "a".
	class VariableExprAST : public ExprAST {
		std::string Name;

	   public:
		VariableExprAST(const std::string& Name) : Name(Name) {}

		Value* codegen() override;
	};

	/// BinaryExprAST - Expression class for a binary operator.
	class BinaryExprAST : public ExprAST {
		char Op;
		std::unique_ptr<ExprAST> LHS, RHS;

	   public:
		BinaryExprAST(char Op, std::unique_ptr<ExprAST> LHS, std::unique_ptr<ExprAST> RHS)
			: Op(Op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}

		Value* codegen() override;
	};

	/// CallExprAST - Expression class for function calls.
	class CallExprAST : public ExprAST {
		std::string Callee;
		std::vector<std::unique_ptr<ExprAST>> Args;

	   public:
		CallExprAST(const std::string& Callee, std::vector<std::unique_ptr<ExprAST>> Args)
			: Callee(Callee), Args(std::move(Args)) {}

		Value* codegen() override;
	};

	/// PrototypeAST - This class represents the "prototype" for a function,
	/// which captures its name, and its argument names (thus implicitly the number
	/// of arguments the function takes).
	class PrototypeAST {
		std::string Name;
		std::vector<std::string> Args;

	   public:
		PrototypeAST(const std::string& Name, std::vector<std::string> Args)
			: Name(Name), Args(std::move(Args)) {}

		Function* codegen();
		const std::string& getName() const { return Name; }
	};

	/// FunctionAST - This class represents a function definition itself.
	class FunctionAST {
		std::unique_ptr<PrototypeAST> Proto;
		std::unique_ptr<ExprAST> Body;

	   public:
		FunctionAST(std::unique_ptr<PrototypeAST> Proto, std::unique_ptr<ExprAST> Body)
			: Proto(std::move(Proto)), Body(std::move(Body)) {}

		Function* codegen();
	};

}  // end anonymous namespace

//===----------------------------------------------------------------------===//
// Parser
//===----------------------------------------------------------------------===//

/// CurTok/getNextToken - Provide a simple token buffer.  CurTok is the current
/// token the parser is looking at.  getNextToken reads another token from the
/// lexer and updates CurTok with its results.
static int CurTok;
static int getNextToken() { return CurTok = gettok(); }

/// BinopPrecedence - This holds the precedence for each binary operator that is
/// defined.
static std::map<char, int> BinopPrecedence;

/// GetTokPrecedence - Get the precedence of the pending binary operator token.
static int GetTokPrecedence() {
	if (!isascii(CurTok))
		return -1;

	// Make sure it's a declared binop.
	int TokPrec = BinopPrecedence[CurTok];
	if (TokPrec <= 0)
		return -1;
	return TokPrec;
}

/// LogError* - These are little helper functions for error handling.
std::unique_ptr<ExprAST> LogError(const char* fmt, ...) {
	printf("Error: ");
	va_list args;
	va_start(args, fmt);
	printf(fmt, args);
	va_end(args);
	printf("\n");
	return nullptr;
}

std::unique_ptr<PrototypeAST> LogErrorP(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	LogError(fmt, args);
	va_end(args);
	return nullptr;
}

static std::unique_ptr<ExprAST> ParseExpression();

/// numberexpr ::= number
static std::unique_ptr<ExprAST> ParseNumberExpr() {
	auto Result = std::make_unique<NumberExprAST>(NumVal);
	getNextToken();	 // consume the number
	return std::move(Result);
}

/// parenexpr ::= '(' expression ')'
static std::unique_ptr<ExprAST> ParseParenExpr() {
	getNextToken();	 // eat (.
	auto V = ParseExpression();
	if (!V)
		return nullptr;

	if (CurTok != ')')
		return LogError("expected ')', read %c", CurTok);
	getNextToken();	 // eat ).
	return V;
}

/// identifierexpr
///   ::= identifier
///   ::= identifier '(' expression* ')'
static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
	std::string IdName = IdentifierStr;

	getNextToken();	 // eat identifier.

	if (CurTok != '(')	// Simple variable ref.
		return std::make_unique<VariableExprAST>(IdName);

	// Call.
	getNextToken();	 // eat (
	std::vector<std::unique_ptr<ExprAST>> Args;
	if (CurTok != ')') {
		while (true) {
			if (auto Arg = ParseExpression())
				Args.push_back(std::move(Arg));
			else
				return nullptr;

			if (CurTok == ')')
				break;

			if (CurTok != ',')
				return LogError("Expected ')' or ',' in argument list, read %i", CurTok);
			getNextToken();
		}
	}

	// Eat the ')'.
	getNextToken();

	return std::make_unique<CallExprAST>(IdName, std::move(Args));
}

/// primary
///   ::= identifierexpr
///   ::= numberexpr
///   ::= parenexpr
static std::unique_ptr<ExprAST> ParsePrimary() {
	switch (CurTok) {
		case tok_identifier:
			return ParseIdentifierExpr();
		case tok_number:
			return ParseNumberExpr();
		case '(':
			return ParseParenExpr();
		default:
			return LogError("unknown token %i when expecting an expression", CurTok);
	}
}

/// binoprhs
///   ::= ('+' primary)*
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec, std::unique_ptr<ExprAST> LHS) {
	// If this is a binop, find its precedence.
	while (true) {
		int TokPrec = GetTokPrecedence();

		// If this is a binop that binds at least as tightly as the current binop,
		// consume it, otherwise we are done.
		if (TokPrec < ExprPrec)
			return LHS;

		// Okay, we know this is a binop.
		int BinOp = CurTok;
		getNextToken();	 // eat binop

		// Parse the primary expression after the binary operator.
		auto RHS = ParsePrimary();
		if (!RHS)
			return nullptr;

		// If BinOp binds less tightly with RHS than the operator after RHS, let
		// the pending operator take RHS as its LHS.
		int NextPrec = GetTokPrecedence();
		if (TokPrec < NextPrec) {
			RHS = ParseBinOpRHS(TokPrec + 1, std::move(RHS));
			if (!RHS)
				return nullptr;
		}

		// Merge LHS/RHS.
		LHS = std::make_unique<BinaryExprAST>(BinOp, std::move(LHS), std::move(RHS));
	}
}

/// expression
///   ::= primary binoprhs
///
static std::unique_ptr<ExprAST> ParseExpression() {
	auto LHS = ParsePrimary();
	if (!LHS)
		return nullptr;

	return ParseBinOpRHS(0, std::move(LHS));
}

/// prototype
///   ::= id '(' id* ')'
static std::unique_ptr<PrototypeAST> ParsePrototype() {
	if (CurTok != tok_identifier)
		return LogErrorP("Expected function name in prototype but got %i", CurTok);

	std::string FnName = IdentifierStr;
	getNextToken();

	if (CurTok != '(')
		return LogErrorP("Expected '(' in prototype but got %i", CurTok);

	std::vector<std::string> ArgNames;
	while (getNextToken() == tok_identifier)
		ArgNames.push_back(IdentifierStr);
	if (CurTok != ')')
		return LogErrorP("Expected ')' in prototype but got %i", CurTok);

	// success.
	getNextToken();	 // eat ')'.

	return std::make_unique<PrototypeAST>(FnName, std::move(ArgNames));
}

/// definition ::= 'def' prototype expression
static std::unique_ptr<FunctionAST> ParseDefinition() {
	getNextToken();	 // eat def.
	auto Proto = ParsePrototype();
	if (!Proto)
		return nullptr;

	if (auto E = ParseExpression())
		return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
	return nullptr;
}

/// toplevelexpr ::= expression
static std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
	if (auto E = ParseExpression()) {
		// Make an anonymous proto.
		auto Proto = std::make_unique<PrototypeAST>("__anon_expr", std::vector<std::string>());
		return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
	}
	return nullptr;
}

/// external ::= 'extern' prototype
static std::unique_ptr<PrototypeAST> ParseExtern() {
	getNextToken();	 // eat extern.
	return ParsePrototype();
}

//===----------------------------------------------------------------------===//
// Code Generation
//===----------------------------------------------------------------------===//

static std::unique_ptr<LLVMContext> TheContext;
static std::unique_ptr<Module> TheModule;
static std::unique_ptr<IRBuilder<>> Builder;
static std::map<std::string, Value*> NamedValues;

Value* LogErrorV(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	LogError(fmt, args);
	va_end(args);
	return nullptr;
}

Value* NumberExprAST::codegen() {
	return ConstantFP::get(*TheContext, APFloat(Val));
}

Value* VariableExprAST::codegen() {
	// Look this variable up in the function.
	Value* V = NamedValues[Name];
	if (!V)
		return LogErrorV("Unknown variable name %s", Name.c_str());
	return V;
}

Value* BinaryExprAST::codegen() {
	Value* L = LHS->codegen();
	Value* R = RHS->codegen();
	if (!L || !R)
		return nullptr;

	switch (Op) {
		case '+':
			return Builder->CreateFAdd(L, R, "addtmp");
		case '-':
			return Builder->CreateFSub(L, R, "subtmp");
		case '*':
			return Builder->CreateFMul(L, R, "multmp");
		case '<':
			L = Builder->CreateFCmpULT(L, R, "cmptmp");
			// Convert bool 0/1 to double 0.0 or 1.0
			return Builder->CreateUIToFP(L, Type::getDoubleTy(*TheContext), "booltmp");
		default:
			return LogErrorV("invalid binary operator %c", Op);
	}
}

Value* CallExprAST::codegen() {
	// Look up the name in the global module table.
	Function* CalleeF = TheModule->getFunction(Callee);
	if (!CalleeF)
		return LogErrorV("Unknown function %s referenced", Callee.c_str());

	// If argument mismatch error.
	if (CalleeF->arg_size() != Args.size())
		return LogErrorV("Incorrect # arguments passed, expected %u, passed %u", Args.size(), CalleeF->arg_size());

	std::vector<Value*> ArgsV;
	for (unsigned i = 0, e = Args.size(); i != e; ++i) {
		ArgsV.push_back(Args[i]->codegen());
		if (!ArgsV.back())
			return nullptr;
	}

	return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
}

Function* PrototypeAST::codegen() {
	// Make the function type:  double(double,double) etc.
	std::vector<Type*> Doubles(Args.size(), Type::getDoubleTy(*TheContext));
	FunctionType* FT = FunctionType::get(Type::getDoubleTy(*TheContext), Doubles, false);

	Function* F = Function::Create(FT, Function::ExternalLinkage, Name, TheModule.get());

	// Set names for all arguments.
	unsigned Idx = 0;
	for (auto& Arg : F->args())
		Arg.setName(Args[Idx++]);

	return F;
}

Function* FunctionAST::codegen() {
	// First, check for an existing function from a previous 'extern' declaration.
	Function* TheFunction = TheModule->getFunction(Proto->getName());

	if (!TheFunction)
		TheFunction = Proto->codegen();

	if (!TheFunction)
		return nullptr;

	// Create a new basic block to start insertion into.
	BasicBlock* BB = BasicBlock::Create(*TheContext, "entry", TheFunction);
	Builder->SetInsertPoint(BB);

	// Record the function arguments in the NamedValues map.
	NamedValues.clear();
	for (auto& Arg : TheFunction->args())
		NamedValues[std::string(Arg.getName())] = &Arg;

	if (Value* RetVal = Body->codegen()) {
		// Finish off the function.
		Builder->CreateRet(RetVal);

		// Validate the generated code, checking for consistency.
		verifyFunction(*TheFunction);

		return TheFunction;
	}

	// Error reading body, remove function.
	TheFunction->eraseFromParent();
	return nullptr;
}

//===----------------------------------------------------------------------===//
// Top-Level parsing and JIT Driver
//===----------------------------------------------------------------------===//

static void InitializeModule() {
	// Open a new context and module.
	TheContext = std::make_unique<LLVMContext>();
	TheModule = std::make_unique<Module>(file_name.c_str(), *TheContext);

	// Create a new builder for the module.
	Builder = std::make_unique<IRBuilder<>>(*TheContext);
}

static void HandleDefinition() {
	if (auto FnAST = ParseDefinition()) {
		if (auto* FnIR = FnAST->codegen()) {
			printf("Read function definition:\n");
			FnIR->print(outs());
		}
	} else {
		// Skip token for error recovery.
		getNextToken();
	}
}

static void HandleExtern() {
	if (auto ProtoAST = ParseExtern()) {
		if (auto* FnIR = ProtoAST->codegen()) {
			printf("Read extern:\n");
			FnIR->print(outs());
		}
	} else {
		// Skip token for error recovery.
		getNextToken();
	}
}

static void HandleTopLevelExpression() {
	// Evaluate a top-level expression into an anonymous function.
	if (auto FnAST = ParseTopLevelExpr()) {
		if (auto* FnIR = FnAST->codegen()) {
			printf("Read top-level expression:\n");
			FnIR->print(outs());

			// Remove the anonymous expression.
			FnIR->eraseFromParent();
		}
	} else {
		// Skip token for error recovery.
		getNextToken();
	}
}

/// top ::= definition | external | expression | ';'
static void MainLoop() {
	while (true) {
		switch (CurTok) {
			case tok_eof:
				return;
			case ';':  // ignore top-level semicolons.
				getNextToken();
				break;
			case tok_function:
				HandleDefinition();
				break;
			case tok_extern:
				HandleExtern();
				break;
			default:
				HandleTopLevelExpression();
				break;
		}
	}
}

//===----------------------------------------------------------------------===//
// Main driver code.
//===----------------------------------------------------------------------===//

int main(int argc, char* argv[]) {
	// Install standard binary operators.
	// 1 is lowest precedence.
	BinopPrecedence['<'] = 10;
	BinopPrecedence['+'] = 20;
	BinopPrecedence['-'] = 20;
	BinopPrecedence['*'] = 40;	// highest.

	const std::string extension = ".tp";
	if (argc == 1) {
		// No file specified
		// compile all files with file extension

		// Find all files in the current path with the extension
		for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::current_path())) {
			if (entry.path().extension().compare(extension) == 0) {
				printf("Compiling %s\n", entry.path().c_str());
				// Open the file
				file = std::ifstream(entry.path());
				if (!file.is_open()) printf("Failed to open %s\n", entry.path().c_str());

				file_name = entry.path().filename();

				getNextToken();

				// Make the module, which holds all the code.
				InitializeModule();

				printf("Starting main loop\n");

				// Run the main "interpreter loop" now.
				MainLoop();

				// Print out all of the generated code.
				TheModule->print(errs(), nullptr);

				if (file.is_open()) file.close();
			}
		}
	} else {
		for (size_t i = 1; i < argc - 1; i++) {
			// Open argv[i]
			// compile argv[i]
		}
	}
}

/*
# Compile
clang++ -g -O3 topami.cpp dreamberd.cpp -I. `llvm-config --cxxflags --ldflags --system-libs --libs core` -o topami
# Run and save IR to file (by redirect stderr)
./topami 2> ./topami.ll
*/