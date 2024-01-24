#!/bin/bash

gcc main.c -o test
echo "Running Test Case 1..."
./test << EOF
insert 1 aaa bbb
insert 2 ccc ddd
select
.exit
EOF

echo "Running Test Case 2..."
string1=$(printf 'A%.0s' {1..25})
string2=$(printf 'B%.0s' {1..36})
./test << EOF
insert 1 $string1 bbb
insert 2 ccc $string2
select
.exit
EOF

echo "Running Test Case 3..."
./test << EOF
insert 1 1 1
insert 2 2 2
insert 3 3 3
insert 4 4 4
insert 5 5 5
insert 6 6 6
insert 7 7 7
insert 8 8 8
insert 9 9 9
insert 10 10 10
insert 11 11 11
insert 12 12 12
insert 13 13 13
insert 14 14 14
insert 15 15 15
insert 16 16 16
insert 17 17 17
insert 18 18 18
insert 19 19 19
insert 20 20 20
insert 21 21 21
insert 22 22 22
insert 23 23 23
insert 24 24 24
insert 25 25 25
insert 26 26 26
insert 27 27 27
insert 28 28 28
insert 29 29 29
insert 30 30 30
insert 31 31 31
insert 32 32 32
insert 33 33 33
insert 34 34 34
insert 35 35 35
insert 36 36 36
insert 37 37 37
insert 38 38 38
insert 39 39 39
insert 40 40 40
insert 41 41 41
insert 42 42 42
insert 43 43 43
insert 44 44 44
insert 45 45 45
insert 46 46 46
insert 47 47 47
insert 48 48 48
insert 49 49 49
insert 50 50 50
insert 51 51 51
insert 52 52 52
insert 53 53 53
insert 54 54 54
insert 55 55 55
insert 56 56 56
insert 57 57 57
insert 58 58 58
insert 59 59 59
insert 60 60 60
insert 61 61 61
insert 62 62 62
insert 63 63 63
insert 64 64 64
insert 65 65 65
insert 66 66 66
insert 67 67 67
insert 68 68 68
insert 69 69 69
insert 70 70 70
insert 71 71 71
insert 72 72 72
insert 73 73 73
insert 74 74 74
insert 75 75 75
insert 76 76 76
insert 77 77 77
insert 78 78 78
insert 79 79 79
insert 80 80 80
insert 81 81 81
insert 82 82 82
insert 83 83 83
insert 84 84 84
insert 85 85 85
insert 86 86 86
insert 87 87 87
insert 88 88 88
insert 89 89 89
insert 90 90 90
insert 91 91 91
insert 92 92 92
insert 93 93 93
insert 94 94 94
insert 95 95 95
insert 96 96 96
insert 97 97 97
insert 98 98 98
insert 99 99 99
insert 100 100 100
select
.exit
EOF

echo "Test End"
rm test