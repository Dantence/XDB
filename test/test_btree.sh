#!/bin/bash

# 执行程序的命令（示例：假设要执行的程序名为example_program）
gcc ../main.c -o test
program_command="./test test.db"

# 模拟输入命令
input_commands="
insert 1 aa aa
insert 2 cc cc
insert 3 bb bb
insert 4 cc cc
insert 5 cc cc
insert 6 cc cc
insert 7 cc cc
insert 8 cc cc
insert 9 cc cc
insert 10 cc cc
insert 11 cc cc
insert 12 cc cc
insert 13 cc cc
insert 14 cc cc
.btree
.exit"

# 执行程序并模拟输入命令，并将输出保存到变量中
actual_output=$(echo -e "$input_commands" | $program_command)
echo "$actual_output"
echo "Test End"
rm test
rm test.db

