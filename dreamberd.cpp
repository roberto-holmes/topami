#include "dreamberd.hpp"

namespace dreamberd {
	bool is_function_definition(std::string& token) {
		if (token.length() < 2) return false;
		const std::string function_word = "function";

		std::string::size_type pos = 0;

		for (char c : token) {
			pos = function_word.find(c, pos);
			if (pos == std::string::npos) return false;
			pos++;
		}
		return true;
	}
}  // namespace dreamberd