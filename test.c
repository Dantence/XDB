// test.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "db.h"

#define NUM_OPERATIONS 450  // 测试的操作数量

void measure_time(const char *operation, void (*func)(void)) {
    LARGE_INTEGER start, end, freq;
    double cpu_time_used;

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    func();
    QueryPerformanceCounter(&end);

    cpu_time_used = (double)(end.QuadPart - start.QuadPart) / freq.QuadPart;
    printf("%s took %f seconds to execute\n", operation, cpu_time_used);
}

Database db;
InputBuffer* input_buffer;

void execute_sql(const char* query) {
    strncpy(input_buffer->buffer, query, input_buffer->buffer_length - 1);
    input_buffer->buffer[input_buffer->buffer_length - 1] = '\0';
    input_buffer->input_length = strlen(query);

    Statement statement;
    if (prepare_statement(input_buffer, &statement, &db) == PREPARE_SUCCESS) {
        execute_statement(&statement, &db);
    } else {
        printf("Error preparing statement: %s\n", query);
    }
}

void test_insert() {
    char query[256];
    for (int i = 1; i < NUM_OPERATIONS; i++) {
        snprintf(query, sizeof(query), "insert into test_table (id, col1, col2) values (%d, text%d, %d);", i, i, i);
        execute_sql(query);
    }
}

void test_update() {
    char query[256];
    for (int i = 1; i < NUM_OPERATIONS; i++) {
        snprintf(query, sizeof(query), "update test_table set col1 = new_text%d, col2 = %d where id = %d;", i, i + 1, i);
        execute_sql(query);
    }
}

void test_select() {
    char query[256];
    for (int i = 1; i < NUM_OPERATIONS; i++) {
        snprintf(query, sizeof(query), "select * from test_table where id = %d;", i);
        execute_sql(query);
    }
}

void test_delete() {
    char query[256];
    for (int i = 1; i < NUM_OPERATIONS; i++) {
        snprintf(query, sizeof(query), "delete from test_table where id = %d;", i);
        execute_sql(query);
    }
}

int main2() {
    // 初始化数据库
    char* db_file = "test.db";
    db.table_count = 0;
    load_database(&db, db_file);  // Load the database state
    input_buffer = new_input_buffer();

    execute_sql("create table test_table (id int, col1 text, col2 int);");

    measure_time("Insert", test_insert);
    measure_time("Update", test_update);
    measure_time("Select", test_select);
    measure_time("Delete", test_delete);

    // 清理数据库
    execute_sql("drop table test_table;");

    save_database(&db, db_file);  // Ensure database is saved on normal exit
    close_input_buffer(input_buffer);

    return 0;
}
