// clang -O3 -emit-llvm hello.c -c -o hello.bc

#include <stdio.h>

int main()
{
    printf("hello world\n");
    return 0;
}