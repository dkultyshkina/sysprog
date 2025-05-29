GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant

all: clean
	gcc $(GCC_FLAGS) solution.c parser.c ../utils/unit.c ../utils/heap_help/heap_help.c  -o a.out

# For automatic testing systems to be able to just build whatever was submitted
# by a student.
test_glob:
	gcc $(GCC_FLAGS) *.c -o mybash

clean:
	rm -rf a.out
