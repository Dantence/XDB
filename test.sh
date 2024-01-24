#!/bin/bash

gcc main.c -o test
echo "Running Test Case 1... (basic function test)"
./test << EOF
insert 1 aaa bbb
insert 2 ccc ddd
select
.exit
EOF

echo "Running Test Case 2... (string length test)"
string1=$(printf 'A%.0s' {1..33})
string2=$(printf 'B%.0s' {1..100})
./test << EOF
insert 1 $string1 bbb
insert 2 ccc $string2
select
.exit
EOF

echo "Running Test Case 3... (max row test)"
./test <<EOF
$(for i in {1..1300}; do
    echo "insert $i $i $i"
done)
select
insert 1301 aa bb
.exit
EOF


echo "Test End"
rm test