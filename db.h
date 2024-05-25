// database.h
#ifndef DATABASE_H
#define DATABASE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#define MAX_TABLE_NAME_LENGTH 64
#define TABLE_MAX_PAGES 100
#define TABLE_MAX_COLS 10
#define INVALID_PAGE_NUM UINT32_MAX
#define OUTPUT_BUFFER_SIZE 8192
#define INPUT_BUFFER_SIZE 1024

typedef enum {
    COLUMN_TYPE_INT,
    COLUMN_TYPE_DOUBLE,
    COLUMN_TYPE_TEXT
} ColumnType;

typedef struct {
    char name[32];
    ColumnType type;
} Column;

typedef struct {
    char name[32];
    int num_columns;
    Column columns[TABLE_MAX_COLS];
    uint32_t row_size;
    uint32_t leaf_node_cell_size;
    uint32_t leaf_node_space_for_cells;
    uint32_t leaf_node_max_cells;
    uint32_t leaf_node_left_split_count;
} TableSchema;

typedef struct {
    uint32_t id;
    char* columns[TABLE_MAX_COLS];
} Row;

typedef struct {
    char* buffer;
    size_t buffer_length;
    ssize_t input_length;
} InputBuffer;

typedef enum {
    EXECUTE_SUCCESS,
    EXECUTE_TABLE_FULL,
    EXECUTE_DUPLICATE_KEY,
    EXECUTE_TABLE_NOT_FOUND,
    EXECUTE_FAILURE
} ExecuteResult;

typedef enum {
    META_COMMAND_SUCCESS,
    META_COMMAND_UNRECOGNIZED_COMMAND
} MetaCommandResult;

typedef enum {
    PREPARE_SUCCESS,
    PREPARE_SYNTAX_ERROR,
    PREPARE_STRING_TOO_LONG,
    PREPARE_UNRECOGNIZED_STATEMENT,
    PREPARE_NEGATIVE_ID,
    PREPARE_TABLE_NOT_FOUND
} PrepareResult;

typedef struct {
    int file_descriptor;
    uint32_t file_length;
    uint32_t num_pages;
    void* pages[TABLE_MAX_PAGES];
} Pager;

typedef struct {
    uint32_t root_page_num;
    Pager* pager;
    TableSchema schema;
} Table;

typedef struct {
    Table* tables[100];
    int table_count;
} Database;

typedef struct {
    Table* table;
    uint32_t page_num;
    uint32_t cell_num;
    bool end_of_table;
} Cursor;

typedef enum {
    STATEMENT_INSERT,
    STATEMENT_SELECT,
    STATEMENT_UPDATE,
    STATEMENT_DELETE,
    STATEMENT_CREATE_TABLE,
    STATEMENT_DROP_TABLE,
    STATEMENT_SHOW_TABLES,
    STATEMENT_DESC_TABLE
} StatementType;

typedef struct {
    StatementType type;
    char table_name[32];
    int num_columns;
    Column columns[TABLE_MAX_COLS];
    Row row_to_insert;
    char condition_column[32];
    char condition_operator[3];
    char condition_value[32];
    bool has_condition;
} Statement;

void append_to_output_buffer(const char *format, ...);
InputBuffer* new_input_buffer();
void close_input_buffer(InputBuffer* input_buffer);
PrepareResult prepare_statement(InputBuffer* input_buffer, Statement* statement, Database* db);
ExecuteResult execute_statement(Statement* statement, Database* db);
void load_database(Database* db, const char* db_file);
void save_database(Database* db, const char* db_file);

#endif // DATABASE_H
