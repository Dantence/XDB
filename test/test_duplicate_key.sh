#!/bin/bash

# 执行程序的命令（示例：假设要执行的程序名为example_program）
gcc ../main.c -o test
program_command="./test test.db"

# 期望的输出字符串（可能有多行）
expected_output="db > Executed.
db > Executed.
db > Executed.
db > Error: Duplicate key."

# 模拟输入命令
input_commands="insert 1 aa aa
insert 3 cc cc
insert 2 bb bb
insert 3 cc cc
.exit"

# 执行程序并模拟输入命令，并将输出保存到变量中
actual_output=$(echo -e "$input_commands" | $program_command)

echo "Test End"
rm test
rm test.db

# 判断输出是否与给定的字符串匹配
if echo "$actual_output" | grep -q "$expected_output"; then
  echo "Test success!。"
else
  echo "Test failure!"
fi
