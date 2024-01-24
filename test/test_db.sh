#!/bin/bash

gcc ../main.c -o test
./test test.db << EOF
insert 1 aaa bbb
insert 2 ccc ddd
select
.exit
EOF

./test test.db << EOF
insert 3 eee fff
select
.exit
EOF

echo "Test End"
rm test
rm test.db