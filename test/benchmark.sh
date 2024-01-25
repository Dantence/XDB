#!/bin/bash
gcc ../main.c -o a

# 测试数据库文件名
filename="benchmark.db"

# 数据库操作命令文件名
commands_file="commands.txt"
num_instructions=1000
# 生成测试命令文件
for ((i=1; i<=$num_instructions; i++))
do
  echo "insert $i aaa bbb" >> $commands_file
done

echo "select" >> $commands_file

echo ".exit" >> $commands_file

# 执行多次测试
num_iterations=10
total_execution_time=0

declare -a execution_times

for ((i=1; i<=$num_iterations; i++))
do
    echo "开始第 $i 次测试..."

    start_time=$(date +%s%N)

    # 执行命令
    ./a $filename < $commands_file

    end_time=$(date +%s%N)

    # 计算总时间
    execution_time=$((($end_time - $start_time)/1000000))

    execution_times+=($execution_time)
    total_execution_time=$((total_execution_time + execution_time))
    rm $filename
done

# 计算平均执行时间
average_execution_time=$((total_execution_time / num_iterations))
echo "平均执行时间: $average_execution_time 毫秒"

# 输出每次测试的执行时间
echo "每次测试的执行时间:"
for ((i=0; i<$num_iterations; i++))
do
    echo "第 $((i+1)) 次测试: ${execution_times[$i]} 毫秒"
done

# 删除测试文件
rm $commands_file
rm a