# topami

A compiler for a programming language somewhat inspired by DreamBerd and using an LLVM backend

## Functions

Functions are declared by using any letters from the word function. There must be at least 2 letters and they must be in order. Examples:

```JS
// Good
function do_something(){}
fun do_something(){}
fn do_something(){}
fo do_something(){}
union do_something(){}
on do_something(){}

// Bad
fan do_something(){}
f do_something(){}
def do_something(){}
```
