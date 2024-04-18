#!/bin/bash

# 执行程序的命令（示例：假设要执行的程序名为example_program）
gcc ../main.c -o test
program_command="./test test.db"

# 模拟输入命令
input_commands="
insert 18 user18 person18@example.com
insert 7 user7 person7@example.com
insert 10 user10 person10@example.com
insert 29 user29 person29@example.com
insert 23 user23 person23@example.com
insert 4 user4 person4@example.com
insert 14 user14 person14@example.com
insert 30 user30 person30@example.com
insert 15 user15 person15@example.com
insert 26 user26 person26@example.com
insert 22 user22 person22@example.com
insert 19 user19 person19@example.com
insert 2 user2 person2@example.com
insert 1 user1 person1@example.com
insert 21 user21 person21@example.com
insert 11 user11 person11@example.com
insert 6 user6 person6@example.com
insert 20 user20 person20@example.com
insert 5 user5 person5@example.com
insert 8 user8 person8@example.com
insert 9 user9 person9@example.com
insert 3 user3 person3@example.com
insert 12 user12 person12@example.com
insert 27 user27 person27@example.com
insert 17 user17 person17@example.com
insert 16 user16 person16@example.com
insert 13 user13 person13@example.com
insert 24 user24 person24@example.com
insert 25 user25 person25@example.com
insert 28 user28 person28@example.com
.btree
.exit
"

# 执行程序并模拟输入命令，并将输出保存到变量中
actual_output=$(echo -e "$input_commands" | $program_command)
echo "$actual_output"
echo "Test End"
rm test
rm test.db

