GCC_FLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant

all: clean test

test:
	gcc $(GCC_FLAGS) userfs.c test.c ../utils/unit.c ../utils/heap_help/heap_help.c  -I ../utils -o test

# For automatic testing systems to be able to just build whatever was submitted
# by a student.
test_glob:
	gcc $(GCC_FLAGS) *.c ../utils/unit.c -I ../utils -o test

clean:
	rm -rf test
