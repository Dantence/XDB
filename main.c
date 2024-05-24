#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <io.h>
#include <gtk/gtk.h>
#include <stdarg.h>
#include <pango/pango.h>
#include <string.h>
#include <errno.h>

#define MAX_TABLE_NAME_LENGTH 64
#define TABLE_MAX_PAGES 100
#define TABLE_MAX_COLS 10
#define INVALID_PAGE_NUM UINT32_MAX

#define OUTPUT_BUFFER_SIZE 8192
char output_buffer[OUTPUT_BUFFER_SIZE];
size_t output_buffer_pos = 0;

void append_to_output_buffer(const char *format, ...) {
    va_list args;
    va_start(args, format);
    output_buffer_pos += vsnprintf(output_buffer + output_buffer_pos, OUTPUT_BUFFER_SIZE - output_buffer_pos, format, args);
    va_end(args);
}

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
    Column columns[10]; // 假设最多支持10列
    uint32_t row_size; // 行大小
    uint32_t leaf_node_cell_size; // 叶节点单元大小
    uint32_t leaf_node_space_for_cells; // 叶节点单元空间
    uint32_t leaf_node_max_cells; // 叶节点最大单元数
    uint32_t leaf_node_left_split_count; // 左分裂单元数
} TableSchema;

// 行属性
typedef struct {
    uint32_t id;
    char* columns[10]; // 动态列值，最多支持10列
} Row;

#define INPUT_BUFFER_SIZE 1024
// 输入信息
typedef struct {
    char* buffer;
    size_t buffer_length;
    ssize_t input_length;
} InputBuffer;

// 创建输入信息
InputBuffer* new_input_buffer() {
    InputBuffer* input_buffer = (InputBuffer*)malloc(sizeof(InputBuffer));
    if (!input_buffer) {
        perror("Unable to allocate input buffer");
        exit(EXIT_FAILURE);
    }
    input_buffer->buffer = (char*)malloc(INPUT_BUFFER_SIZE);
    if (!input_buffer->buffer) {
        perror("Unable to allocate input buffer");
        free(input_buffer);
        exit(EXIT_FAILURE);
    }
    input_buffer->buffer_length = INPUT_BUFFER_SIZE;
    input_buffer->input_length = 0;
    return input_buffer;
}

// 自定义 getline 实现
ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    if (*lineptr == NULL || *n == 0) {
        *n = 128; // 初始缓冲区大小
        *lineptr = (char*)malloc(*n);
        if (*lineptr == NULL) {
            return -1; // 分配失败
        }
    }

    char *buffer = *lineptr;
    int ch;
    size_t i = 0;

    while ((ch = fgetc(stream)) != EOF) {
        if (i >= *n - 1) { // 扩展缓冲区
            *n *= 2;
            buffer = (char*)realloc(buffer, *n);
            if (buffer == NULL) {
                return -1; // 分配失败
            }
            *lineptr = buffer;
        }
        buffer[i++] = ch;
        if (ch == '\n') {
            break;
        }
    }

    if (ch == EOF && i == 0) {
        return -1; // EOF 并且没有读取到任何字符
    }

    buffer[i] = '\0';
    return i;
}

// 关闭input_buffer
void close_input_buffer(InputBuffer* input_buffer) {
    free(input_buffer->buffer);
    free(input_buffer);
}


// 执行结果
typedef enum {
    EXECUTE_SUCCESS,
    EXECUTE_TABLE_FULL,
    EXECUTE_DUPLICATE_KEY,
    EXECUTE_TABLE_NOT_FOUND,
    EXECUTE_FAILURE
} ExecuteResult;

// 元命令，以.开头
typedef enum {
    META_COMMAND_SUCCESS,
    META_COMMAND_UNRECOGNIZED_COMMAND
} MetaCommandResult;

// SQL语句
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
    TableSchema schema; // 新增：表架构
} Table;


typedef struct {
    Table* tables[100]; // 最多支持100张表
    int table_count;
} Database;

// 游标抽象
typedef struct {
    Table* table;
    uint32_t page_num;
    uint32_t cell_num;
    bool end_of_table; // 标识表末尾
} Cursor;

// SQL语句类型
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

// SQL语句
typedef struct {
    StatementType type;
    char table_name[32];
    int num_columns;          // 列数
    Column columns[TABLE_MAX_COLS];       // 列信息
    Row row_to_insert;        // 仅用于插入语句
    char condition_column[32];
    char condition_operator[3];
    char condition_value[32];
    bool has_condition;
} Statement;

// 打印行
void print_row(Row* row, TableSchema* schema) {
    char buffer[1024];  // 临时缓冲区来存储格式化后的字符串
    int offset = snprintf(buffer, sizeof(buffer), "(%d", row->id);

    for (int i = 1; i < schema->num_columns; i++) {  // 从 0 开始
        switch (schema->columns[i].type) {
            case COLUMN_TYPE_INT:
                if (row->columns[i] != NULL) {
                    offset += snprintf(buffer + offset, sizeof(buffer) - offset, ", %d", *((int*)row->columns[i]));
                } else {
                    offset += snprintf(buffer + offset, sizeof(buffer) - offset, ", NULL");
                }
            break;
            case COLUMN_TYPE_DOUBLE:
                if (row->columns[i] != NULL) {
                    offset += snprintf(buffer + offset, sizeof(buffer) - offset, ", %.2f", *((double*)row->columns[i]));
                } else {
                    offset += snprintf(buffer + offset, sizeof(buffer) - offset, ", NULL");
                }
            break;
            case COLUMN_TYPE_TEXT:
                if (row->columns[i] != NULL) {
                    if (!g_utf8_validate(row->columns[i], -1, NULL)) {
                        // 如果字符串无效，则替换为有效的指示字符串
                        offset += snprintf(buffer + offset, sizeof(buffer) - offset, ", %s", "<Invalid UTF-8>");
                    } else {
                        offset += snprintf(buffer + offset, sizeof(buffer) - offset, ", %s", row->columns[i]);
                    }
                } else {
                    offset += snprintf(buffer + offset, sizeof(buffer) - offset, ", NULL");
                }
            break;
        }
    }

    snprintf(buffer + offset, sizeof(buffer) - offset, ")\n");

    // 将结果追加到输出缓冲区
    append_to_output_buffer("%s", buffer);
}



// 打印提示符
void print_prompt() {
    append_to_output_buffer("db > ");
    printf("db > ");
}

// 各字段size和offset
#define size_of_attribute(Struct, Attribute) sizeof(((Struct*)0)->Attribute)

const uint32_t ID_SIZE = sizeof(uint32_t);
const uint32_t ID_OFFSET = 0;
const uint32_t PAGE_SIZE = 4096;

// B+树节点类型
typedef enum { NODE_INTERNAL, NODE_LEAF } NodeType;

// 节点头布局
const uint32_t NODE_TYPE_SIZE = sizeof(uint8_t);
const uint32_t NODE_TYPE_OFFSET = 0;
const uint32_t IS_ROOT_SIZE = sizeof(uint8_t);
const uint32_t IS_ROOT_OFFSET = NODE_TYPE_SIZE;
const uint32_t PARENT_POINTER_SIZE = sizeof(uint32_t);
const uint32_t PARENT_POINTER_OFFSET = IS_ROOT_OFFSET + IS_ROOT_SIZE;
const uint8_t COMMON_NODE_HEADER_SIZE =
        NODE_TYPE_SIZE + IS_ROOT_SIZE + PARENT_POINTER_SIZE;

// 叶节点头布局
const uint32_t LEAF_NODE_NUM_CELLS_SIZE = sizeof(uint32_t);
const uint32_t LEAF_NODE_NUM_CELLS_OFFSET = COMMON_NODE_HEADER_SIZE;
const uint32_t LEAF_NODE_NEXT_LEAF_SIZE = sizeof(uint32_t);
const uint32_t LEAF_NODE_NEXT_LEAF_OFFSET =
        LEAF_NODE_NUM_CELLS_OFFSET + LEAF_NODE_NUM_CELLS_SIZE;
const uint32_t LEAF_NODE_HEADER_SIZE = COMMON_NODE_HEADER_SIZE
                                       + LEAF_NODE_NUM_CELLS_SIZE
                                       + LEAF_NODE_NEXT_LEAF_SIZE;

// 内部节点头部布局
const uint32_t INTERNAL_NODE_NUM_KEYS_SIZE = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_NUM_KEYS_OFFSET = COMMON_NODE_HEADER_SIZE;
const uint32_t INTERNAL_NODE_RIGHT_CHILD_SIZE = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_RIGHT_CHILD_OFFSET =
INTERNAL_NODE_NUM_KEYS_OFFSET + INTERNAL_NODE_NUM_KEYS_SIZE;
const uint32_t INTERNAL_NODE_HEADER_SIZE = COMMON_NODE_HEADER_SIZE +
INTERNAL_NODE_NUM_KEYS_SIZE +
INTERNAL_NODE_RIGHT_CHILD_SIZE;

// 内部节点体布局
// 内部是一个单元格数组，其中每个单元格都包含一个子指针和一个键。每个键都应该是其左侧子项中包含的最大键。
const uint32_t INTERNAL_NODE_KEY_SIZE = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_CHILD_SIZE = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_CELL_SIZE =
        INTERNAL_NODE_CHILD_SIZE + INTERNAL_NODE_KEY_SIZE;
const uint32_t INTERNAL_NODE_MAX_CELLS = 3;

NodeType get_node_type(void* node) {
    uint8_t value = *((uint8_t*)(node + NODE_TYPE_OFFSET));
    return (NodeType)value;
}

void set_node_type(void* node, NodeType type) {
    uint8_t value = type;
    *((uint8_t*)(node + NODE_TYPE_OFFSET)) = value;
}

uint32_t* leaf_node_num_cells(void* node) {
    return node + LEAF_NODE_NUM_CELLS_OFFSET;
}

void* leaf_node_cell(void* node, uint32_t cell_num, TableSchema* schema) {
    return node + LEAF_NODE_HEADER_SIZE + cell_num * schema->leaf_node_cell_size;
}

uint32_t* leaf_node_key(void* node, uint32_t cell_num, TableSchema* schema) {
    return (uint32_t*)(leaf_node_cell(node, cell_num, schema));
}

void* leaf_node_value(void* node, uint32_t cell_num, TableSchema* schema) {
    return leaf_node_cell(node, cell_num, schema) + sizeof(uint32_t);
}

uint32_t* leaf_node_next_leaf(void* node) {
    return node + LEAF_NODE_NEXT_LEAF_OFFSET;
}

uint32_t* node_parent(void* node) {
    return node + PARENT_POINTER_OFFSET;
}

// 假设在具有 N 页的数据库中，分配了页码 0 到 N-1。因此，我们始终可以为新页面分配页码 N
uint32_t get_unused_page_num(Pager* pager) {
    return pager->num_pages;
}

uint32_t* internal_node_num_keys(void* node) {
    return node + INTERNAL_NODE_NUM_KEYS_OFFSET;
}

uint32_t* internal_node_right_child(void* node) {
    return node + INTERNAL_NODE_RIGHT_CHILD_OFFSET;
}

uint32_t* internal_node_cell(void* node, uint32_t cell_num) {
    return node + INTERNAL_NODE_HEADER_SIZE + cell_num * INTERNAL_NODE_CELL_SIZE;
}

uint32_t* internal_node_child(void* node, uint32_t child_num) {
    uint32_t num_keys = *internal_node_num_keys(node);
    if (child_num > num_keys) {
        printf("Tried to access child_num %d > num_keys %d\n", child_num, num_keys);
        exit(EXIT_FAILURE);
    } else if (child_num == num_keys) {
        uint32_t* right_child = internal_node_right_child(node);
        if (*right_child == INVALID_PAGE_NUM) {
            printf("Tried to access right child of node, but was invalid page\n");
            exit(EXIT_FAILURE);
        }
        return right_child;
    } else {
        uint32_t* child = internal_node_cell(node, child_num);
        if (*child == INVALID_PAGE_NUM) {
            printf("Tried to access child %d of node, but was invalid page\n", child_num);
            exit(EXIT_FAILURE);
        }
        return child;
    }
}

uint32_t* internal_node_key(void* node, uint32_t key_num) {
    return (void*)internal_node_cell(node, key_num) + INTERNAL_NODE_CHILD_SIZE;
}

// 对于内部节点，最大键始终是其右键。对于叶节点，它是最大索引处的键
//uint32_t get_node_max_key(void* node) {
//    switch (get_node_type(node)) {
//        case NODE_INTERNAL:
//            return *internal_node_key(node, *internal_node_num_keys(node) - 1);
//        case NODE_LEAF:
//            return *leaf_node_key(node, *leaf_node_num_cells(node) - 1);
//    }
//}

bool is_node_root(void* node) {
    uint8_t value = *((uint8_t*)(node + IS_ROOT_OFFSET));
    return (bool)value;
}

void set_node_root(void* node, bool is_root) {
    uint8_t value = is_root;
    *((uint8_t*)(node + IS_ROOT_OFFSET)) = value;
}

void initialize_leaf_node(void* node, TableSchema* schema) {
    *leaf_node_num_cells(node) = 0;
    set_node_root(node, false);
    set_node_type(node, NODE_LEAF);
    *leaf_node_next_leaf(node) = 0;
    memcpy(node + LEAF_NODE_HEADER_SIZE, schema, sizeof(TableSchema)); // 存储表架构
}

void initialize_internal_node(void* node) {
    set_node_type(node, NODE_INTERNAL);
    set_node_root(node, false);
    *internal_node_num_keys(node) = 0;
    *internal_node_right_child(node) = INVALID_PAGE_NUM;
}

// 动态计算表结构相关常量
void calculate_table_constants(TableSchema* schema) {
    uint32_t row_size = sizeof(uint32_t); // id 大小
    for (int i = 0; i < schema->num_columns; i++) {
        switch (schema->columns[i].type) {
            case COLUMN_TYPE_INT:
                row_size += sizeof(int);
            break;
            case COLUMN_TYPE_DOUBLE:
                row_size += sizeof(double);
            break;
            case COLUMN_TYPE_TEXT:
                row_size += 255; // 假设最大长度为255
            break;
        }
    }

    schema->row_size = row_size;
    schema->leaf_node_cell_size = sizeof(uint32_t) + row_size;
    schema->leaf_node_space_for_cells = 4096 - LEAF_NODE_HEADER_SIZE;
    schema->leaf_node_max_cells = schema->leaf_node_space_for_cells / schema->leaf_node_cell_size;
    schema->leaf_node_left_split_count = (schema->leaf_node_max_cells + 1) / 2;
}


void serialize_row(Row* source, void* destination, TableSchema* schema) {
    memcpy(destination, &(source->id), sizeof(uint32_t));
    uint32_t offset = sizeof(uint32_t);
    for (int i = 0; i < schema->num_columns; i++) {
        switch (schema->columns[i].type) {
            case COLUMN_TYPE_INT:
                if (source->columns[i] != NULL) {
                    memcpy(destination + offset, source->columns[i], sizeof(int));
                } else {
                    int default_int = 0;
                    memcpy(destination + offset, &default_int, sizeof(int));
                }
            offset += sizeof(int);
            break;
            case COLUMN_TYPE_DOUBLE:
                if (source->columns[i] != NULL) {
                    memcpy(destination + offset, source->columns[i], sizeof(double));
                } else {
                    double default_double = 0.0;
                    memcpy(destination + offset, &default_double, sizeof(double));
                }
            offset += sizeof(double);
            break;
            case COLUMN_TYPE_TEXT:
                if (source->columns[i] != NULL) {
                    strcpy(destination + offset, source->columns[i]);
                    offset += strlen(source->columns[i]) + 1;
                } else {
                    char default_text = '\0';
                    memcpy(destination + offset, &default_text, 1);
                    offset += 1;
                }
            break;
        }
    }
}


void deserialize_row(void* source, Row* destination, TableSchema* schema) {
    memcpy(&(destination->id), source, sizeof(uint32_t));
    uint32_t offset = sizeof(uint32_t);
    for (int i = 0; i < schema->num_columns; i++) {
        switch (schema->columns[i].type) {
            case COLUMN_TYPE_INT:
                destination->columns[i] = malloc(sizeof(int));
            memcpy(destination->columns[i], source + offset, sizeof(int));
            offset += sizeof(int);
            break;
            case COLUMN_TYPE_DOUBLE:
                destination->columns[i] = malloc(sizeof(double));
            memcpy(destination->columns[i], source + offset, sizeof(double));
            offset += sizeof(double);
            break;
            case COLUMN_TYPE_TEXT:
                destination->columns[i] = strdup(source + offset);
            offset += strlen(source + offset) + 1;
            break;
        }
    }
}

// 获取页面
void* get_page(Pager* pager, uint32_t page_num) {
    if (page_num > TABLE_MAX_PAGES) {
        printf("Tried to fetch page number out of bounds. %d > %d\n",
               page_num, TABLE_MAX_PAGES);
        exit(EXIT_FAILURE);
    }
    if (pager->pages[page_num] == NULL) {
        // 缓存未命中，分配内存
        void* page = malloc(PAGE_SIZE);
        uint32_t num_pages = pager->file_length / PAGE_SIZE;
        // 未满页
        if (pager->file_length % PAGE_SIZE) {
            num_pages += 1;
        }
        // 如果请求的页面位于文件的范围之外，我们知道它应该是空白的，所以我们只需分配一些内存并返回它。稍后将缓存刷新到磁盘时，该页面将被添加到文件中。
        if (page_num <= num_pages) {
            lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);
            ssize_t bytes_read = read(pager->file_descriptor, page, PAGE_SIZE);
            if (bytes_read == -1) {
                printf("Error reading file: %d\n", errno);
                exit(EXIT_FAILURE);
            }
        }
        pager->pages[page_num] = page;
        if (page_num >= pager->num_pages) {
            pager->num_pages = page_num + 1;
        }
    }
    return pager->pages[page_num];
}


uint32_t get_node_max_key(Pager* pager, void* node, TableSchema* schema) {
    if (get_node_type(node) == NODE_LEAF) {
        return *leaf_node_key(node, *leaf_node_num_cells(node) - 1, schema);
    }
    void* right_child = get_page(pager, *internal_node_right_child(node));
    return get_node_max_key(pager, right_child, schema);
}


void pager_flush(Pager* pager, uint32_t page_num) {
    if (pager->pages[page_num] == NULL) {
        printf("Tried to flush null page\n");
        exit(EXIT_FAILURE);
    }

    off_t offset = lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);
    if (offset == -1) {
        printf("Error seeking: %d\n", errno);
        exit(EXIT_FAILURE);
    }

    ssize_t bytes_written = write(pager->file_descriptor, pager->pages[page_num], PAGE_SIZE);
    if (bytes_written == -1) {
        printf("Error writing: %d\n", errno);
        exit(EXIT_FAILURE);
    }
}

void table_close(Table* table) {
    Pager* pager = table->pager;
    // 释放缓存
    for (uint32_t i = 0; i < pager->num_pages; i++) {
        if (pager->pages[i] == NULL) {
            continue;
        }
        pager_flush(pager, i);
        free(pager->pages[i]);
        pager->pages[i] = NULL;
    }
    // 关闭文件
    int result = close(pager->file_descriptor);
    if (result == -1) {
        printf("Error closing db file.\n");
        exit(EXIT_FAILURE);
    }

    // 释放pager
    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        void* page = pager->pages[i];
        if (page) {
            free(page);
            pager->pages[i] = NULL;
        }
    }

    // 释放table
    free(pager);
    free(table);
}

void print_constants(TableSchema* schema) {
    printf("ROW_SIZE: %d\n", schema->row_size);
    printf("LEAF_NODE_CELL_SIZE: %d\n", schema->leaf_node_cell_size);
    printf("LEAF_NODE_SPACE_FOR_CELLS: %d\n", schema->leaf_node_space_for_cells);
    printf("LEAF_NODE_MAX_CELLS: %d\n", schema->leaf_node_max_cells);
    printf("LEAF_NODE_LEFT_SPLIT_COUNT: %d\n", schema->leaf_node_left_split_count);
}

void indent(uint32_t level) {
    for (uint32_t i = 0; i < level; i++) {
        printf("  ");
    }
}

// B树可视化
void print_tree(Pager* pager, uint32_t page_num, uint32_t indentation_level, TableSchema* schema) {
    void* node = get_page(pager, page_num);
    uint32_t num_keys, child;

    switch (get_node_type(node)) {
        case (NODE_LEAF):
            num_keys = *leaf_node_num_cells(node);
        indent(indentation_level);
        printf("- leaf (size %d)\n", num_keys);
        for (uint32_t i = 0; i < num_keys; i++) {
            indent(indentation_level + 1);
            printf("- %d\n", *leaf_node_key(node, i, schema));
        }
        break;
        case (NODE_INTERNAL):
            num_keys = *internal_node_num_keys(node);
        indent(indentation_level);
        printf("- internal (size %d)\n", num_keys);
        if (num_keys > 0) {
            for (uint32_t i = 0; i < num_keys; i++) {
                child = *internal_node_child(node, i);
                print_tree(pager, child, indentation_level + 1, schema);

                indent(indentation_level + 1);
                printf("- key %d\n", *internal_node_key(node, i));
            }
            child = *internal_node_right_child(node);
            print_tree(pager, child, indentation_level + 1, schema);
        }
        break;
    }
}


Cursor* leaf_node_find(Table* table, uint32_t page_num, uint32_t key) {
    void* node = get_page(table->pager, page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    Cursor* cursor = malloc(sizeof(Cursor));
    cursor->table = table;
    cursor->page_num = page_num;

    // Binary search
    uint32_t min_index = 0;
    uint32_t one_past_max_index = num_cells;
    while (one_past_max_index != min_index) {
        uint32_t index = (min_index + one_past_max_index) / 2;
        uint32_t key_at_index = *leaf_node_key(node, index, &table->schema);
        if (key == key_at_index) {
            cursor->cell_num = index;
            return cursor;
        }
        if (key < key_at_index) {
            one_past_max_index = index;
        } else {
            min_index = index + 1;
        }
    }

    cursor->cell_num = min_index;
    return cursor;
}


uint32_t internal_node_find_child(void* node, uint32_t key) {
    uint32_t num_keys = *internal_node_num_keys(node);

    /* Binary search to find index of child to search */
    uint32_t min_index = 0;
    uint32_t max_index = num_keys; /* there is one more child than key */

    while (min_index != max_index) {
        uint32_t index = (min_index + max_index) / 2;
        uint32_t key_to_right = *internal_node_key(node, index);
        if (key_to_right >= key) {
            max_index = index;
        } else {
            min_index = index + 1;
        }
    }
    return min_index;
}

Cursor* internal_node_find(Table* table, uint32_t page_num, uint32_t key) {
    void* node = get_page(table->pager, page_num);
    uint32_t child_index = internal_node_find_child(node, key);
    uint32_t child_num = *internal_node_child(node, child_index);
    void* child = get_page(table->pager, child_num);
    switch (get_node_type(child)) {
        case NODE_LEAF:
            return leaf_node_find(table, child_num, key);
        case NODE_INTERNAL:
            return internal_node_find(table, child_num, key);
    }
}

Cursor* table_find(Table* table, uint32_t key) {
    uint32_t root_page_num = table->root_page_num;
    void* root_node = get_page(table->pager, root_page_num);

    if (get_node_type(root_node) == NODE_LEAF) {
        return leaf_node_find(table, root_page_num, key);
    } else {
        return internal_node_find(table, root_page_num, key);
    }
}

//Cursor* table_start(Table* table) {
//    Cursor* cursor = malloc(sizeof(Cursor));
//    cursor->table = table;
//    cursor->cell_num = 0;
//    cursor->page_num = table->root_page_num;
//    void* root_node = get_page(table->pager, table->root_page_num);
//    uint32_t num_cells = *leaf_node_num_cells(root_node);
//    cursor->end_of_table = (num_cells == 0);
//
//    return cursor;
//}

// 搜索键 0（最小可能键）。即使表中不存在键 0，此方法也会返回最低 id 的位置（最左边叶节点的起点）。
Cursor* table_start(Table* table) {
    Cursor* cursor = table_find(table, 0);

    void* node = get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    cursor->end_of_table = (num_cells == 0);

    return cursor;
}

// 获取指向光标所描述位置的指针
void* cursor_value(Cursor* cursor) {
    uint32_t page_num = cursor->page_num;
    void* page = get_page(cursor->table->pager, page_num);
    return leaf_node_value(page, cursor->cell_num, &cursor->table->schema);
}

// 每当我们想将光标移过叶节点的末尾时，
// 都可以检查叶节点是否有同级节点
void cursor_advance(Cursor* cursor) {
    uint32_t page_num = cursor->page_num;
    void* node = get_page(cursor->table->pager, page_num);
    cursor->cell_num += 1;
    if (cursor->cell_num >= (*leaf_node_num_cells(node))) {
        /* Advance to next leaf node */
        uint32_t next_page_num = *leaf_node_next_leaf(node);
        if (next_page_num == 0) {
            /* This was rightmost leaf */
            cursor->end_of_table = true;
        } else {
            cursor->page_num = next_page_num;
            cursor->cell_num = 0;
        }
    }
}

/* 处理根节点的分裂。
 * 旧根节点复制到新页，成为左子节点。
 * 右子节点的地址被传递进来。
 * 重新初始化根页以包含新的根节点。
 * 新的根节点指向两个子节点。
*/
void create_new_root(Table* table, uint32_t right_child_page_num) {
    void* root = get_page(table->pager, table->root_page_num);
    void* right_child = get_page(table->pager, right_child_page_num);
    uint32_t left_child_page_num = get_unused_page_num(table->pager);
    void* left_child = get_page(table->pager, left_child_page_num);
    if (get_node_type(root) == NODE_INTERNAL) {
        initialize_internal_node(right_child);
        initialize_internal_node(left_child);
    }

    /* Left child has data copied from old root */
    memcpy(left_child, root, PAGE_SIZE);
    set_node_root(left_child, false);

    if (get_node_type(left_child) == NODE_INTERNAL) {
        void* child;
        for (int i = 0; i < *internal_node_num_keys(left_child); i++) {
            child = get_page(table->pager, *internal_node_child(left_child, i));
            *node_parent(child) = left_child_page_num;
        }
        child = get_page(table->pager, *internal_node_right_child(left_child));
        *node_parent(child) = left_child_page_num;
    }

    /* Root node is a new internal node with one key and two children */
    initialize_internal_node(root);
    set_node_root(root, true);
    *internal_node_num_keys(root) = 1;
    *internal_node_child(root, 0) = left_child_page_num;
    uint32_t left_child_max_key = get_node_max_key(table->pager, left_child, &table->schema);
    *internal_node_key(root, 0) = left_child_max_key;
    *internal_node_right_child(root) = right_child_page_num;
    *node_parent(left_child) = table->root_page_num;
    *node_parent(right_child) = table->root_page_num;
}


void update_internal_node_key(void* node, uint32_t old_key, uint32_t new_key) {
    uint32_t old_child_index = internal_node_find_child(node, old_key);
    *internal_node_key(node, old_child_index) = new_key;
}

void internal_node_split_and_insert(Table* table, uint32_t parent_page_num,
                          uint32_t child_page_num);

void internal_node_insert(Table* table, uint32_t parent_page_num,
                          uint32_t child_page_num) {
    void* parent = get_page(table->pager, parent_page_num);
    void* child = get_page(table->pager, child_page_num);
    uint32_t child_max_key = get_node_max_key(table->pager, child, &table->schema);
    uint32_t index = internal_node_find_child(parent, child_max_key);

    uint32_t original_num_keys = *internal_node_num_keys(parent);

    if (original_num_keys >= INTERNAL_NODE_MAX_CELLS) {
        internal_node_split_and_insert(table, parent_page_num, child_page_num);
        return;
    }

    uint32_t right_child_page_num = *internal_node_right_child(parent);
    if (right_child_page_num == INVALID_PAGE_NUM) {
        *internal_node_right_child(parent) = child_page_num;
        return;
    }

    void* right_child = get_page(table->pager, right_child_page_num);
    *internal_node_num_keys(parent) = original_num_keys + 1;

    if (child_max_key > get_node_max_key(table->pager, right_child, &table->schema)) {
        /* Replace right child */
        *internal_node_child(parent, original_num_keys) = right_child_page_num;
        *internal_node_key(parent, original_num_keys) =
                get_node_max_key(table->pager, right_child, &table->schema);
        *internal_node_right_child(parent) = child_page_num;
    } else {
        /* Make room for the new cell */
        for (uint32_t i = original_num_keys; i > index; i--) {
            void* destination = internal_node_cell(parent, i);
            void* source = internal_node_cell(parent, i - 1);
            memcpy(destination, source, INTERNAL_NODE_CELL_SIZE);
        }
        *internal_node_child(parent, index) = child_page_num;
        *internal_node_key(parent, index) = child_max_key;
    }
}


void internal_node_split_and_insert(Table* table, uint32_t parent_page_num,
                                    uint32_t child_page_num) {
    uint32_t old_page_num = parent_page_num;
    void* old_node = get_page(table->pager, parent_page_num);
    uint32_t old_max = get_node_max_key(table->pager, old_node, &table->schema);

    void* child = get_page(table->pager, child_page_num);
    uint32_t child_max = get_node_max_key(table->pager, child, &table->schema);

    uint32_t new_page_num = get_unused_page_num(table->pager);

    uint32_t splitting_root = is_node_root(old_node);

    void* parent;
    void* new_node;
    if (splitting_root) {
        create_new_root(table, new_page_num);
        parent = get_page(table->pager, table->root_page_num);

        old_page_num = *internal_node_child(parent, 0);
        old_node = get_page(table->pager, old_page_num);
    } else {
        parent = get_page(table->pager, *node_parent(old_node));
        new_node = get_page(table->pager, new_page_num);
        initialize_internal_node(new_node);
    }

    uint32_t* old_num_keys = internal_node_num_keys(old_node);

    uint32_t cur_page_num = *internal_node_right_child(old_node);
    void* cur = get_page(table->pager, cur_page_num);

    internal_node_insert(table, new_page_num, cur_page_num);
    *node_parent(cur) = new_page_num;
    *internal_node_right_child(old_node) = INVALID_PAGE_NUM;

    for (int i = INTERNAL_NODE_MAX_CELLS - 1; i > INTERNAL_NODE_MAX_CELLS / 2; i--) {
        cur_page_num = *internal_node_child(old_node, i);
        cur = get_page(table->pager, cur_page_num);

        internal_node_insert(table, new_page_num, cur_page_num);
        *node_parent(cur) = new_page_num;

        (*old_num_keys)--;
    }

    *internal_node_right_child(old_node) = *internal_node_child(old_node, *old_num_keys - 1);
    (*old_num_keys)--;

    uint32_t max_after_split = get_node_max_key(table->pager, old_node, &table->schema);

    uint32_t destination_page_num = (child_max < max_after_split) ? old_page_num : new_page_num;

    internal_node_insert(table, destination_page_num, child_page_num);
    *node_parent(child) = destination_page_num;

    update_internal_node_key(parent, old_max, get_node_max_key(table->pager, old_node, &table->schema));

    if (!splitting_root) {
        internal_node_insert(table, *node_parent(old_node), new_page_num);
        *node_parent(new_node) = *node_parent(old_node);
    }
}


// 分裂操作：分配一个新的叶节点，并将较大的一半移动到新节点中。
void leaf_node_split_and_insert(Cursor* cursor, uint32_t key, Row* value) {
    void* old_node = get_page(cursor->table->pager, cursor->page_num);
    uint32_t old_max = get_node_max_key(cursor->table->pager, old_node, &cursor->table->schema);
    uint32_t new_page_num = get_unused_page_num(cursor->table->pager);
    void* new_node = get_page(cursor->table->pager, new_page_num);
    initialize_leaf_node(new_node, &cursor->table->schema);
    *node_parent(new_node) = *node_parent(old_node);
    *leaf_node_next_leaf(new_node) = *leaf_node_next_leaf(old_node);
    *leaf_node_next_leaf(old_node) = new_page_num;

    // 拷贝和插入新值
    for (int32_t i = cursor->table->schema.leaf_node_max_cells; i >= 0; i--) {
        void* destination_node;
        if (i >= cursor->table->schema.leaf_node_left_split_count) {
            destination_node = new_node;
        } else {
            destination_node = old_node;
        }

        uint32_t index_within_node = i % cursor->table->schema.leaf_node_left_split_count;
        void* destination = leaf_node_cell(destination_node, index_within_node, &cursor->table->schema);

        if (i == cursor->cell_num) {
            serialize_row(value, leaf_node_value(destination_node, index_within_node, &cursor->table->schema), &cursor->table->schema);
            *leaf_node_key(destination_node, index_within_node, &cursor->table->schema) = key;
        } else if (i > cursor->cell_num) {
            memcpy(destination, leaf_node_cell(old_node, i - 1, &cursor->table->schema), cursor->table->schema.leaf_node_cell_size);
        } else {
            memcpy(destination, leaf_node_cell(old_node, i, &cursor->table->schema), cursor->table->schema.leaf_node_cell_size);
        }
    }

    // 更新单元格数量
    *(leaf_node_num_cells(old_node)) = cursor->table->schema.leaf_node_left_split_count;
    *(leaf_node_num_cells(new_node)) = cursor->table->schema.leaf_node_max_cells - cursor->table->schema.leaf_node_left_split_count + 1;

    if (is_node_root(old_node)) {
        create_new_root(cursor->table, new_page_num);
    } else {
        uint32_t parent_page_num = *node_parent(old_node);
        uint32_t new_max = get_node_max_key(cursor->table->pager, old_node, &cursor->table->schema);
        void* parent = get_page(cursor->table->pager, parent_page_num);
        update_internal_node_key(parent, old_max, new_max);
        internal_node_insert(cursor->table, parent_page_num, new_page_num);
    }
}



void leaf_node_insert(Cursor* cursor, uint32_t key, Row* value) {
    void* node = get_page(cursor->table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    if (num_cells >= cursor->table->schema.leaf_node_max_cells) {
        leaf_node_split_and_insert(cursor, key, value);
        return;
    }
    if(cursor->cell_num < num_cells) {
        for (uint32_t i = num_cells; i > cursor->cell_num; i--) {
            memcpy(leaf_node_cell(node, i, &cursor->table->schema), leaf_node_cell(node, i - 1, &cursor->table->schema), cursor->table->schema.leaf_node_cell_size);
        }
    }
    *(leaf_node_num_cells(node)) += 1;
    *(leaf_node_key(node, cursor->cell_num, &cursor->table->schema)) = key;
    serialize_row(value, leaf_node_value(node, cursor->cell_num, &cursor->table->schema), &cursor->table->schema);
}


Table* find_table(Database* db, const char* table_name) {
    for (int i = 0; i < db->table_count; i++) {
        if (strcmp(db->tables[i]->schema.name, table_name) == 0) {
            return db->tables[i];
        }
    }
    return NULL;
}

// SQL compiler
PrepareResult prepare_insert(InputBuffer* input_buffer, Statement* statement, Database* db) {
    statement->type = STATEMENT_INSERT;

    char* buffer = strdup(input_buffer->buffer);
    char* keyword = strtok(buffer, " ");
    char* into = strtok(NULL, " ");
    char* table_name = strtok(NULL, " ");

    if (strcmp(into, "into") != 0 || table_name == NULL) {
        free(buffer);
        return PREPARE_SYNTAX_ERROR;
    }

    Table* table = find_table(db, table_name);
    if (table == NULL) {
        free(buffer);
        return PREPARE_TABLE_NOT_FOUND;
    }

    char* column_list_start = strstr(input_buffer->buffer, "(");
    char* column_list_end = strstr(input_buffer->buffer, ")");
    char* values_keyword = strstr(input_buffer->buffer, "values");
    if (column_list_start == NULL || column_list_end == NULL || values_keyword == NULL) {
        free(buffer);
        return PREPARE_SYNTAX_ERROR;
    }

    if (values_keyword < column_list_end) {
        free(buffer);
        return PREPARE_SYNTAX_ERROR;
    }

    // Extract and parse column list
    size_t column_list_length = column_list_end - column_list_start - 1;
    char* column_list = (char*)malloc(column_list_length + 1);
    strncpy(column_list, column_list_start + 1, column_list_length);
    column_list[column_list_length] = '\0';

    // Extract and parse value list
    char* value_list_start = strstr(values_keyword, "(");
    char* value_list_end = strstr(values_keyword, ")");
    if (value_list_start == NULL || value_list_end == NULL) {
        free(buffer);
        free(column_list);
        return PREPARE_SYNTAX_ERROR;
    }

    size_t value_list_length = value_list_end - value_list_start - 1;
    char* value_list = (char*)malloc(value_list_length + 1);
    strncpy(value_list, value_list_start + 1, value_list_length);
    value_list[value_list_length] = '\0';

    // Parse columns
    char* column = strtok(column_list, ",");
    int column_indices[TABLE_MAX_COLS];
    int column_count = 0;

    while (column != NULL) {
        // Trim leading whitespace
        while (*column == ' ') column++;
        // Validate and store column index
        bool found = false;
        for (int i = 0; i < table->schema.num_columns; i++) {
            if (strcmp(column, table->schema.columns[i].name) == 0) {
                found = true;
                column_indices[column_count++] = i;
                break;
            }
        }
        if (!found) {
            free(buffer);
            free(column_list);
            free(value_list);
            return PREPARE_SYNTAX_ERROR;  // Invalid column name
        }
        column = strtok(NULL, ",");
    }

    // Initialize all columns to NULL or default values
    for (int i = 0; i < table->schema.num_columns; i++) {
        statement->row_to_insert.columns[i] = NULL;
    }

    // Parse values
    char* value = strtok(value_list, ",");
    int value_index = 0;
    while (value != NULL && value_index < column_count) {
        // Trim leading whitespace
        while (*value == ' ') value++;
        // Convert and store value
        int col_index = column_indices[value_index];
        if (table->schema.columns[col_index].type == COLUMN_TYPE_INT) {
            if (col_index == 0) {
                int id = atoi(value);
                statement->row_to_insert.id = id;
                if (id < 0) {
                    free(buffer);
                    free(column_list);
                    free(value_list);
                    return PREPARE_NEGATIVE_ID;
                }
            }
            int* int_value = malloc(sizeof(int));
            *int_value = atoi(value);
            statement->row_to_insert.columns[col_index] = (char*)int_value;
        } else if (table->schema.columns[col_index].type == COLUMN_TYPE_DOUBLE) {
            double* double_value = malloc(sizeof(double));
            *double_value = atof(value);
            statement->row_to_insert.columns[col_index] = (char*)double_value;
        } else if (table->schema.columns[col_index].type == COLUMN_TYPE_TEXT) {
            if (strlen(value) > 255) {
                free(buffer);
                free(column_list);
                free(value_list);
                return PREPARE_STRING_TOO_LONG;
            }
            statement->row_to_insert.columns[col_index] = strdup(value);
        }
        value = strtok(NULL, ",");
        value_index++;
    }

    strcpy(statement->table_name, table_name);

    free(buffer);
    free(column_list);
    free(value_list);
    return PREPARE_SUCCESS;
}

PrepareResult prepare_select(InputBuffer* input_buffer, Statement* statement, Database* db) {
    statement->type = STATEMENT_SELECT;
    char* buffer = strdup(input_buffer->buffer);
    char* keyword = strtok(buffer, " "); // SELECT
    char* remaining_str = buffer + strlen(keyword) + 1; // Remaining string after SELECT

    // Find the position of "FROM"
    char* from_pos = strstr(remaining_str, " from ");
    if (from_pos == NULL) {
        free(buffer);
        return PREPARE_SYNTAX_ERROR;
    }

    // Extract columns part and table name part
    size_t columns_length = from_pos - remaining_str;
    char* columns_str = (char*)malloc(columns_length + 1);
    strncpy(columns_str, remaining_str, columns_length);
    columns_str[columns_length] = '\0';

    char* table_name = from_pos + strlen(" from ");
    if (table_name == NULL) {
        free(buffer);
        free(columns_str);
        return PREPARE_SYNTAX_ERROR;
    }

    // Find the position of "WHERE"
    char* where_pos = strstr(table_name, " where ");
    if (where_pos != NULL) {
        *where_pos = '\0'; // Null-terminate table name
        where_pos += strlen(" where ");
        // Parse WHERE condition
        char* condition_column = strtok(where_pos, " ");
        char* condition_operator = strtok(NULL, " ");
        char* condition_value = strtok(NULL, " ");

        if (condition_column == NULL || condition_operator == NULL || condition_value == NULL) {
            free(buffer);
            free(columns_str);
            return PREPARE_SYNTAX_ERROR;
        }

        strcpy(statement->condition_column, condition_column);
        strcpy(statement->condition_operator, condition_operator);
        strcpy(statement->condition_value, condition_value);
        statement->has_condition = true;
    } else {
        statement->has_condition = false;
    }

    if (strcmp(columns_str, "*") == 0) {
        statement->num_columns = 0; // Select all columns
    } else {
        char* col = strtok(columns_str, ",");
        while (col != NULL) {
            while (*col == ' ') col++; // Skip leading spaces
            strncpy(statement->columns[statement->num_columns].name, col, 32);
            statement->num_columns++;
            col = strtok(NULL, ",");
        }
    }

    strcpy(statement->table_name, table_name);
    free(buffer);
    free(columns_str);
    return PREPARE_SUCCESS;
}


PrepareResult prepare_update(InputBuffer* input_buffer, Statement* statement, Database* db) {
    statement->type = STATEMENT_UPDATE;
    char* buffer = strdup(input_buffer->buffer);
    char* keyword = strtok(buffer, " ");
    char* table_name = strtok(NULL, " ");
    if (table_name == NULL) {
        free(buffer);
        return PREPARE_SYNTAX_ERROR;
    }
    strcpy(statement->table_name, table_name);

    char* set_keyword = strtok(NULL, " ");
    if (set_keyword == NULL || strcmp(set_keyword, "set") != 0) {
        free(buffer);
        return PREPARE_SYNTAX_ERROR;
    }

    char* set_clause = strtok(NULL, ";");
    if (set_clause == NULL) {
        free(buffer);
        return PREPARE_SYNTAX_ERROR;
    }

    // 解析 set 子句
    char* column_value_pair = strtok(set_clause, ",");
    int column_count = 0;
    while (column_value_pair != NULL && column_count < TABLE_MAX_COLS) {
        // 去除等号两侧的空格
        while (*column_value_pair == ' ') column_value_pair++;
        char* equal_sign = strchr(column_value_pair, '=');
        if (equal_sign == NULL) {
            free(buffer);
            return PREPARE_SYNTAX_ERROR;
        }

        *equal_sign = '\0';
        char* column_name = column_value_pair;
        char* value = equal_sign + 1;

        // 去除列名后面的空格
        char* col_end = column_name + strlen(column_name) - 1;
        while (col_end > column_name && *col_end == ' ') col_end--;
        *(col_end + 1) = '\0';

        // 去除值两侧的空格
        while (*value == ' ') value++;
        char* end = value + strlen(value) - 1;
        while (end > value && *end == ' ') end--;
        *(end + 1) = '\0';

        // 查找列索引
        Table* table = find_table(db, table_name);
        if (!table) {
            free(buffer);
            return PREPARE_TABLE_NOT_FOUND;
        }

        bool found = false;
        for (int i = 0; i < table->schema.num_columns; i++) {
            if (strcmp(column_name, table->schema.columns[i].name) == 0) {
                found = true;
                statement->columns[column_count].type = table->schema.columns[i].type;
                strncpy(statement->columns[column_count].name, column_name, 32);
                switch (table->schema.columns[i].type) {
                    case COLUMN_TYPE_INT:
                        statement->row_to_insert.columns[column_count] = malloc(sizeof(int));
                        *((int*)statement->row_to_insert.columns[column_count]) = atoi(value);
                        break;
                    case COLUMN_TYPE_DOUBLE:
                        statement->row_to_insert.columns[column_count] = malloc(sizeof(double));
                        *((double*)statement->row_to_insert.columns[column_count]) = atof(value);
                        break;
                    case COLUMN_TYPE_TEXT:
                        if (strlen(value) > 255) {
                            free(buffer);
                            return PREPARE_STRING_TOO_LONG;
                        }
                        statement->row_to_insert.columns[column_count] = strdup(value);
                        break;
                }
                break;
            }
        }
        if (!found) {
            free(buffer);
            return PREPARE_SYNTAX_ERROR;
        }

        column_count++;
        column_value_pair = strtok(NULL, ",");
    }
    statement->num_columns = column_count;

    free(buffer);
    return PREPARE_SUCCESS;
}

PrepareResult prepare_delete(InputBuffer* input_buffer, Statement* statement, Database* db) {
    statement->type = STATEMENT_DELETE;

    char* buffer = strdup(input_buffer->buffer);
    char* keyword = strtok(buffer, " "); // "delete"
    char* from = strtok(NULL, " "); // "from"
    char* table_name = strtok(NULL, " "); // table name

    if (strcmp(from, "from") != 0 || table_name == NULL) {
        free(buffer);
        return PREPARE_SYNTAX_ERROR;
    }

    char* where = strtok(NULL, " "); // "where"
    char* id_column = strtok(NULL, " "); // "id"
    char* equals = strtok(NULL, " "); // "="
    char* id_value = strtok(NULL, " "); // ID value

    if (where == NULL || id_column == NULL || equals == NULL || id_value == NULL) {
        free(buffer);
        return PREPARE_SYNTAX_ERROR;
    }

    if (strcmp(where, "where") != 0 || strcmp(id_column, "id") != 0 || strcmp(equals, "=") != 0) {
        free(buffer);
        return PREPARE_SYNTAX_ERROR;
    }

    int id = atoi(id_value);
    if (id < 0) {
        free(buffer);
        return PREPARE_NEGATIVE_ID;
    }

    statement->row_to_insert.id = id;
    strcpy(statement->table_name, table_name);
    free(buffer);
    return PREPARE_SUCCESS;
}


PrepareResult prepare_create_table(InputBuffer* input_buffer, Statement* statement) {
    statement->type = STATEMENT_CREATE_TABLE;

    char* buffer = strdup(input_buffer->buffer); // 复制缓冲区以避免修改原始输入

    // 提取表名
    char* keyword = strtok(buffer, " "); // create
    keyword = strtok(NULL, " "); // table
    char* table_name = strtok(NULL, " ("); // table name

    if (table_name == NULL) {
        free(buffer);
        return PREPARE_SYNTAX_ERROR;
    }

    strcpy(statement->table_name, table_name);

    // 提取列定义部分
    char* columns_def = strtok(NULL, "("); // 获取剩余的字符串部分
    if (columns_def == NULL) {
        free(buffer);
        return PREPARE_SYNTAX_ERROR;
    }

    // 去除末尾的括号
    char* end_parenthesis = strchr(columns_def, ')');
    if (end_parenthesis != NULL) {
        *end_parenthesis = '\0';
    } else {
        free(buffer);
        return PREPARE_SYNTAX_ERROR;
    }
    char *cols[TABLE_MAX_COLS];

    // 分割列定义并解析每个列
    for(char *str = strtok(columns_def, ","); str != NULL; str = strtok(NULL, ",")) {
        cols[statement->num_columns] = str;
        statement->num_columns++;
    }
    for(int i = 0; i < statement->num_columns; i++) {
        char* column_name = strtok(cols[i], " ");
        while(*column_name == ' ') column_name++;
        char*column_type_str = strtok(NULL, " ");
        while(*column_type_str == ' ') column_type_str++;
        if (strcmp(column_type_str, "int") == 0) {
            statement->columns[i].type = COLUMN_TYPE_INT;
        } else if (strcmp(column_type_str, "double") == 0) {
            statement->columns[i].type = COLUMN_TYPE_DOUBLE;
        } else if (strcmp(column_type_str, "text") == 0) {
            statement->columns[i].type = COLUMN_TYPE_TEXT;
        } else {
            free(buffer);
            return PREPARE_SYNTAX_ERROR;
        }
        strncpy(statement->columns[i].name, column_name, 32);

    }

    free(buffer);
    return PREPARE_SUCCESS;
}

PrepareResult prepare_statement(InputBuffer* input_buffer, Statement* statement, Database* db) {
    // 确保所有非元命令以分号结束
    if (input_buffer->buffer[input_buffer->input_length - 1] != ';') {
        return PREPARE_SYNTAX_ERROR;
    }

    // 移除分号，以便后续解析
    input_buffer->buffer[input_buffer->input_length - 1] = '\0';
    input_buffer->input_length--;

    if (strncmp(input_buffer->buffer, "insert", 6) == 0) {
        return prepare_insert(input_buffer, statement, db);
    }

    if (strncmp(input_buffer->buffer, "select", 6) == 0) {
        return prepare_select(input_buffer, statement, db);
    }

    if (strncmp(input_buffer->buffer, "update", 6) == 0) {
        return prepare_update(input_buffer, statement, db);
    }

    if (strncmp(input_buffer->buffer, "delete", 6) == 0) {
        return prepare_delete(input_buffer, statement, db);
    }

    if (strncmp(input_buffer->buffer, "create table", 12) == 0) {
        return prepare_create_table(input_buffer, statement);
    }

    if (strncmp(input_buffer->buffer, "drop table", 10) == 0) {
        statement->type = STATEMENT_DROP_TABLE;
        char* keyword = strtok(input_buffer->buffer, " ");
        keyword = strtok(NULL, " ");
        char* table_name = strtok(NULL, " ");
        if (table_name == NULL) {
            return PREPARE_SYNTAX_ERROR;
        }
        strncpy(statement->table_name, table_name, sizeof(statement->table_name) - 1);
        return PREPARE_SUCCESS;
    }


    if (strcmp(input_buffer->buffer, "show tables") == 0) {
        statement->type = STATEMENT_SHOW_TABLES;
        return PREPARE_SUCCESS;
    }

    if (strncmp(input_buffer->buffer, "desc ", 5) == 0) {
        statement->type = STATEMENT_DESC_TABLE;
        strcpy(statement->table_name, input_buffer->buffer + 5);
        return PREPARE_SUCCESS;
    }

    return PREPARE_UNRECOGNIZED_STATEMENT;
}


// 打开数据库文件并跟踪其大小，页面缓存初始化为NULL
Pager* pager_open(const char* filename) {
    int fd = _open(filename,
                  _O_RDWR |     // 读/写模式
                  _O_CREAT,     // 如果文件不存在则创建
                  _S_IWRITE |   // 用户写权限
                  _S_IREAD      // 用户读权限
    );

    if (fd == -1) {
        printf("Unable to open file\n");
        exit(EXIT_FAILURE);
    }

    // 获取文件长度
    off_t file_length = _lseek(fd, 0, SEEK_END);

    Pager* pager = malloc(sizeof(Pager));
    pager->file_descriptor = fd;
    pager->file_length = file_length;
    pager->num_pages = (file_length / PAGE_SIZE);
    if (file_length % PAGE_SIZE != 0) {
        printf("Db file is not a whole number of pages. Corrupt file.\n");
        exit(EXIT_FAILURE);
    }

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        pager->pages[i] = NULL;
    }

    return pager;
}


// 创建表
Table* table_open(const char* filename, TableSchema* schema) {
    Pager* pager = pager_open(filename);
    Table* table = (Table*)malloc(sizeof(Table));
    table->pager = pager;
    table->root_page_num = 0;
    if (pager->num_pages == 0) {
        void* root_node = get_page(pager, 0);
        initialize_leaf_node(root_node, schema);
        set_node_root(root_node, true);
    }
    table->schema = *schema;
    return table;
}

ExecuteResult execute_create_table(Statement* statement, Database* db) {
    if (db->table_count >= 100) {
        return EXECUTE_TABLE_FULL;
    }

    TableSchema schema;
    strcpy(schema.name, statement->table_name); // 设置表架构中的表名
    schema.num_columns = statement->num_columns;
    for (int i = 0; i < statement->num_columns; i++) {
        schema.columns[i] = statement->columns[i];
    }
    calculate_table_constants(&schema);

    Table* table = table_open(statement->table_name, &schema);
    if (!table) {
        return EXECUTE_FAILURE;
    }

    db->tables[db->table_count] = table; // 直接存储表指针

    db->table_count++;
    return EXECUTE_SUCCESS;
}

void free_table(Table* table) {
    // 释放Pager中的每一页
    for (uint32_t i = 0; i < table->pager->num_pages; i++) {
        if (table->pager->pages[i] != NULL) {
            free(table->pager->pages[i]);
            table->pager->pages[i] = NULL;
        }
    }
    // 关闭文件描述符
    close(table->pager->file_descriptor);
    // 释放Pager结构
    free(table->pager);

    // 释放表结构本身
    free(table);
}



void drop_table(Database* db, const char* table_name) {
    for (int i = 0; i < db->table_count; i++) {
        if (strcmp(db->tables[i]->schema.name, table_name) == 0) {
            free_table(db->tables[i]);
            for (int j = i; j < db->table_count - 1; j++) {
                db->tables[j] = db->tables[j + 1];
            }
            db->tables[db->table_count - 1] = NULL;
            db->table_count--;
            return;
        }
    }
}



ExecuteResult execute_drop_table(Statement* statement, Database* db) {
    Table* table = NULL;
    for (int i = 0; i < db->table_count; i++) {
        if (strcmp(db->tables[i]->schema.name, statement->table_name) == 0) {
            table = db->tables[i];
            break;
        }
    }

    if (table == NULL) {
        return EXECUTE_TABLE_NOT_FOUND;
    }

    drop_table(db, statement->table_name);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_show_tables(Database* db) {
    for (int i = 0; i < db->table_count; i++) {
        printf("%s\n", db->tables[i]->schema.name);
        append_to_output_buffer("%s\n", db->tables[i]->schema.name);
    }
    return EXECUTE_SUCCESS;
}


ExecuteResult execute_insert(Statement* statement, Database* db) {
    Table* table = find_table(db, statement->table_name);
    if (!table) {
        return EXECUTE_TABLE_NOT_FOUND;
    }

    void* node = get_page(table->pager, table->root_page_num);
    uint32_t num_cells = (*leaf_node_num_cells(node));
    Row* row_to_insert = &(statement->row_to_insert);
    uint32_t key_to_insert = row_to_insert->id;
    Cursor* cursor = table_find(table, key_to_insert);
    if (cursor->cell_num < num_cells) {
        uint32_t key_at_index = *leaf_node_key(node, cursor->cell_num, &table->schema);
        if (key_at_index == key_to_insert) {
            return EXECUTE_DUPLICATE_KEY;
        }
    }
    leaf_node_insert(cursor, row_to_insert->id, row_to_insert);
    free(cursor);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_select(Statement* statement, Database* db) {
    Table* table = find_table(db, statement->table_name);
    if (!table) {
        return EXECUTE_TABLE_NOT_FOUND;
    }

    if (statement->has_condition && strcmp(statement->condition_column, "id") == 0) {
        uint32_t key = atoi(statement->condition_value);
        Cursor* cursor = table_find(table, key);
        if (cursor->end_of_table || *leaf_node_key(get_page(table->pager, cursor->page_num), cursor->cell_num, &table->schema) != key) {
            // No matching row found
            free(cursor);
            return EXECUTE_SUCCESS;
        }

        Row row;
        deserialize_row(cursor_value(cursor), &row, &table->schema);
        print_row(&row, &table->schema);
        free(cursor);
        return EXECUTE_SUCCESS;
    }

    Cursor* cursor = table_start(table);
    Row row;
    while (!(cursor->end_of_table)) {
        deserialize_row(cursor_value(cursor), &row, &table->schema);

        bool condition_met = true;
        if (statement->has_condition) {
            // Check condition
            for (int i = 0; i < table->schema.num_columns; i++) {
                if (strcmp(statement->condition_column, table->schema.columns[i].name) == 0) {
                    if (table->schema.columns[i].type == COLUMN_TYPE_INT) {
                        int column_value = *((int*)row.columns[i]);
                        int condition_value = atoi(statement->condition_value);
                        if (strcmp(statement->condition_operator, "=") == 0 && column_value != condition_value) {
                            condition_met = false;
                        } else if (strcmp(statement->condition_operator, "<") == 0 && column_value >= condition_value) {
                            condition_met = false;
                        } else if (strcmp(statement->condition_operator, ">") == 0 && column_value <= condition_value) {
                            condition_met = false;
                        }
                        // Add more operators as needed
                    }
                    // Add conditions for other types (DOUBLE, TEXT) as needed
                }
            }
        }

        if (condition_met) {
            if (statement->num_columns == 0) {
                // Select all columns
                print_row(&row, &table->schema);
            } else {
                // Select specific columns
                char buffer[1024];
                int offset = snprintf(buffer, sizeof(buffer), "(%d", row.id);

                for (int i = 0; i < statement->num_columns; i++) {
                    for (int j = 0; j < table->schema.num_columns; j++) {
                        if (strcmp(statement->columns[i].name, table->schema.columns[j].name) == 0) {
                            switch (table->schema.columns[j].type) {
                                case COLUMN_TYPE_INT:
                                    offset += snprintf(buffer + offset, sizeof(buffer) - offset, ", %d", *((int*)row.columns[j]));
                                    break;
                                case COLUMN_TYPE_DOUBLE:
                                    offset += snprintf(buffer + offset, sizeof(buffer) - offset, ", %.2f", *((double*)row.columns[j]));
                                    break;
                                case COLUMN_TYPE_TEXT:
                                    if (!g_utf8_validate(row.columns[j], -1, NULL)) {
                                        offset += snprintf(buffer + offset, sizeof(buffer) - offset, ", %s", "<Invalid UTF-8>");
                                    } else {
                                        offset += snprintf(buffer + offset, sizeof(buffer) - offset, ", %s", row.columns[j]);
                                    }
                                    break;
                            }
                        }
                    }
                }

                snprintf(buffer + offset, sizeof(buffer) - offset, ")\n");
                append_to_output_buffer("%s", buffer);
            }
        }

        cursor_advance(cursor);
    }
    free(cursor);
    return EXECUTE_SUCCESS;
}


ExecuteResult execute_update(Statement* statement, Database* db) {
    Table* table = find_table(db, statement->table_name);
    if (!table) {
        return EXECUTE_TABLE_NOT_FOUND;
    }

    void* node = get_page(table->pager, table->root_page_num);
    uint32_t num_cells = (*leaf_node_num_cells(node));
    Cursor* cursor = table_start(table);
    Row row;

    // 遍历所有行并更新匹配的行
    while (!(cursor->end_of_table)) {
        deserialize_row(cursor_value(cursor), &row, &table->schema);

        // 检查是否满足更新条件 (假设我们有一个简单的条件检查机制)
        // if (row_meets_condition(row)) {
        // 解析并应用set子句的值
        for (int i = 0; i < statement->num_columns; i++) {
            int column_index = -1;
            for (int j = 0; j < table->schema.num_columns; j++) {
                if (strcmp(statement->columns[i].name, table->schema.columns[j].name) == 0) {
                    column_index = j;
                    break;
                }
            }
            if (column_index == -1) {
                continue;
            }
            switch (table->schema.columns[column_index].type) {
                case COLUMN_TYPE_INT:
                    *((int*)row.columns[column_index]) = *((int*)statement->row_to_insert.columns[i]);
                break;
                case COLUMN_TYPE_DOUBLE:
                    *((double*)row.columns[column_index]) = *((double*)statement->row_to_insert.columns[i]);
                break;
                case COLUMN_TYPE_TEXT:
                    free(row.columns[column_index]);
                row.columns[column_index] = strdup(statement->row_to_insert.columns[i]);
                break;
            }
        }
        serialize_row(&row, cursor_value(cursor), &table->schema);
        // }

        cursor_advance(cursor);
    }
    free(cursor);
    return EXECUTE_SUCCESS;
}

ExecuteResult execute_delete(Statement* statement, Database* db) {
    Table* table = find_table(db, statement->table_name);
    if (!table) {
        return EXECUTE_TABLE_NOT_FOUND;
    }

    uint32_t target_id = statement->row_to_insert.id;
    Cursor* cursor = table_find(table, target_id);

    void* node = get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    // 查找目标节点
    bool found = false;
    for (uint32_t i = 0; i < num_cells; i++) {
        uint32_t key = *leaf_node_key(node, i, &table->schema);
        if (key == target_id) {
            found = true;
            // 删除目标节点
            for (uint32_t j = i; j < num_cells - 1; j++) {
                void* destination = leaf_node_cell(node, j, &table->schema);
                void* source = leaf_node_cell(node, j + 1, &table->schema);
                memcpy(destination, source, table->schema.leaf_node_cell_size);
            }
            (*leaf_node_num_cells(node)) -= 1;

            // 更新B树
            if (cursor->page_num != table->root_page_num) {
                uint32_t new_max = get_node_max_key(table->pager, node, &table->schema);
                void* parent = get_page(table->pager, *node_parent(node));
                update_internal_node_key(parent, target_id, new_max);
            }

            break;
        }
    }

    free(cursor);

    if (!found) {
        return EXECUTE_FAILURE;
    }

    return EXECUTE_SUCCESS;
}

ExecuteResult execute_desc_table(Statement* statement, Database* db) {
    Table* table = find_table(db, statement->table_name);
    if (!table) {
        return EXECUTE_TABLE_NOT_FOUND;
    }

    append_to_output_buffer("Table: %s\n", table->schema.name);
    append_to_output_buffer("Columns:\n");
    for (int i = 0; i < table->schema.num_columns; i++) {
        char* type_str;
        switch (table->schema.columns[i].type) {
            case COLUMN_TYPE_INT:
                type_str = "int";
            break;
            case COLUMN_TYPE_DOUBLE:
                type_str = "double";
            break;
            case COLUMN_TYPE_TEXT:
                type_str = "text";
            break;
        }
        append_to_output_buffer("  %s: %s\n", table->schema.columns[i].name, type_str);
    }
    return EXECUTE_SUCCESS;
}


ExecuteResult execute_statement(Statement* statement, Database* db) {
    switch (statement->type) {
        case (STATEMENT_INSERT):
            return execute_insert(statement, db);
        case (STATEMENT_SELECT):
            return execute_select(statement, db);
        case (STATEMENT_UPDATE):
            return execute_update(statement, db);
        case (STATEMENT_DELETE):
            return execute_delete(statement, db);
        case (STATEMENT_CREATE_TABLE):
            return execute_create_table(statement, db);
        case (STATEMENT_DROP_TABLE):
            return execute_drop_table(statement, db);
        case (STATEMENT_SHOW_TABLES):
            return execute_show_tables(db);
        case (STATEMENT_DESC_TABLE):
            return execute_desc_table(statement, db);
        default:
            return EXECUTE_FAILURE;
    }
}

gboolean on_entry_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget));
    if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
        GtkTextIter iter;
        gtk_text_buffer_get_end_iter(buffer, &iter);
        gtk_text_buffer_insert(buffer, &iter, "\n", -1);
        return TRUE; // 阻止默认行为
    }
    return FALSE; // 允许其他按键的默认行为
}

void save_database(Database* db, const char* db_file) {
    char meta_filename[64];
    snprintf(meta_filename, sizeof(meta_filename), "%s.meta", db_file);
    FILE* meta_file = fopen(meta_filename, "wb");
    if (!meta_file) {
        perror("Unable to open metadata file for writing");
        exit(EXIT_FAILURE);
    }

    fwrite(&db->table_count, sizeof(int), 1, meta_file);
    for (int i = 0; i < db->table_count; i++) {
        fwrite(&db->tables[i]->schema, sizeof(TableSchema), 1, meta_file); // 写入表架构
    }
    fclose(meta_file);

    for (int i = 0; i < db->table_count; i++) {
        table_close(db->tables[i]);
    }
}

void load_database(Database* db, const char* db_file) {
    char meta_filename[64];
    snprintf(meta_filename, sizeof(meta_filename), "%s.meta", db_file);
    FILE* meta_file = fopen(meta_filename, "rb");
    if (!meta_file) {
        // No existing database, start fresh
        db->table_count = 0;
        return;
    }

    fread(&db->table_count, sizeof(int), 1, meta_file);
    for (int i = 0; i < db->table_count; i++) {
        db->tables[i] = (Table*)malloc(sizeof(Table)); // 初始化 Table 对象
        fread(&db->tables[i]->schema, sizeof(TableSchema), 1, meta_file); // 读取表架构
        db->tables[i]->pager = pager_open(db->tables[i]->schema.name); // 使用表架构中的表名
        db->tables[i]->root_page_num = 0;
    }
    fclose(meta_file);
}

// 处理元命令
MetaCommandResult do_meta_command(InputBuffer* input_buffer, Database* db, const char* db_file) {
    if (strcmp(input_buffer->buffer, ".exit") == 0) {
        save_database(db, db_file);
        exit(EXIT_SUCCESS);
    } else if (strcmp(input_buffer->buffer, ".constants") == 0) {
        printf("Constants:\n");
        for (int i = 0; i < db->table_count; i++) {
            print_constants(&db->tables[i]->schema);
        }
        return META_COMMAND_SUCCESS;
    } else if (strcmp(input_buffer->buffer, ".btree") == 0) {
        for (int i = 0; i < db->table_count; i++) {
            printf("Table: %s\n", db->tables[i]->schema.name);
            print_tree(db->tables[i]->pager, 0, 0, &db->tables[i]->schema);
        }
        return META_COMMAND_SUCCESS;
    } else {
        return META_COMMAND_UNRECOGNIZED_COMMAND;
    }
}

void on_execute_button_clicked(GtkWidget *widget, gpointer data) {
    GtkWidget **widgets = (GtkWidget **)data;
    GtkWidget *text_view = widgets[0];
    Database *db = (Database *)widgets[1];
    InputBuffer *input_buffer = (InputBuffer *)widgets[2];
    GtkWidget *output_text_view = widgets[3];
    const char *db_file = (const char *)widgets[4];
    GtkWidget *result_view = widgets[5];
    GtkListStore *result_store = GTK_LIST_STORE(widgets[6]);

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(buffer, &start);
    gtk_text_buffer_get_end_iter(buffer, &end);

    gchar *input_text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    strncpy(input_buffer->buffer, input_text, input_buffer->buffer_length - 1);
    input_buffer->buffer[input_buffer->buffer_length - 1] = '\0';
    input_buffer->input_length = strlen(input_text);

    GtkTextBuffer *output_buffer_widget = gtk_text_view_get_buffer(GTK_TEXT_VIEW(output_text_view));
    GtkTextIter output_iter;
    gtk_text_buffer_get_end_iter(output_buffer_widget, &output_iter);
    gtk_text_buffer_insert(output_buffer_widget, &output_iter, "db > ", -1);
    gtk_text_buffer_insert(output_buffer_widget, &output_iter, input_text, -1);
    gtk_text_buffer_insert(output_buffer_widget, &output_iter, "\n", -1);

    g_free(input_text);

    memset(output_buffer, 0, OUTPUT_BUFFER_SIZE);
    output_buffer_pos = 0;

    if (input_buffer->buffer[0] == '.') {
        switch (do_meta_command(input_buffer, db, db_file)) {
            case (META_COMMAND_SUCCESS):
                gtk_text_buffer_insert(output_buffer_widget, &output_iter, "Meta-command executed successfully.\n", -1);
                gtk_text_buffer_insert(output_buffer_widget, &output_iter, output_buffer, -1);
                gtk_text_buffer_set_text(buffer, "", -1);
                return;
            case (META_COMMAND_UNRECOGNIZED_COMMAND):
                gtk_text_buffer_insert(output_buffer_widget, &output_iter, "Unrecognized command.\n", -1);
                gtk_text_buffer_insert(output_buffer_widget, &output_iter, output_buffer, -1);
                return;
        }
    }

    Statement statement;
    switch (prepare_statement(input_buffer, &statement, db)) {
        case (PREPARE_SUCCESS):
            break;
        case (PREPARE_NEGATIVE_ID):
            gtk_text_buffer_insert(output_buffer_widget, &output_iter, "ID must be positive.\n", -1);
            gtk_text_buffer_insert(output_buffer_widget, &output_iter, output_buffer, -1);
            return;
        case (PREPARE_STRING_TOO_LONG):
            gtk_text_buffer_insert(output_buffer_widget, &output_iter, "String is too long.\n", -1);
            gtk_text_buffer_insert(output_buffer_widget, &output_iter, output_buffer, -1);
            return;
        case (PREPARE_SYNTAX_ERROR):
            gtk_text_buffer_insert(output_buffer_widget, &output_iter, "Syntax error. Could not parse statement.\n", -1);
            gtk_text_buffer_insert(output_buffer_widget, &output_iter, output_buffer, -1);
            return;
        case (PREPARE_UNRECOGNIZED_STATEMENT):
            gtk_text_buffer_insert(output_buffer_widget, &output_iter, "Unrecognized keyword.\n", -1);
            gtk_text_buffer_insert(output_buffer_widget, &output_iter, output_buffer, -1);
            return;
    }

    ExecuteResult result = execute_statement(&statement, db);
    if (statement.type == STATEMENT_SELECT && result == EXECUTE_SUCCESS) {
        // 清空旧的列
        GList *columns = gtk_tree_view_get_columns(GTK_TREE_VIEW(result_view));
        for (GList *iter = columns; iter != NULL; iter = iter->next) {
            gtk_tree_view_remove_column(GTK_TREE_VIEW(result_view), GTK_TREE_VIEW_COLUMN(iter->data));
        }
        g_list_free(columns);

        Table *table = find_table(db, statement.table_name);
        if (table) {
            int num_columns = statement.num_columns > 0 ? statement.num_columns : table->schema.num_columns;
            GtkTreeIter tree_iter;
            GType *types = g_new0(GType, num_columns);
            for (int i = 0; i < num_columns; i++) {
                types[i] = G_TYPE_STRING;
                GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
                const char* col_name = statement.num_columns > 0 ? statement.columns[i].name : table->schema.columns[i].name;
                GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(col_name, renderer, "text", i, NULL);
                gtk_tree_view_append_column(GTK_TREE_VIEW(result_view), col);
            }
            result_store = gtk_list_store_newv(num_columns, types);
            g_free(types);

            Cursor *cursor = table_start(table);
            Row row;
            while (!(cursor->end_of_table)) {
                deserialize_row(cursor_value(cursor), &row, &table->schema);

                bool condition_met = true;
                if (statement.has_condition) {
                    // Check condition
                    for (int i = 0; i < table->schema.num_columns; i++) {
                        if (strcmp(statement.condition_column, table->schema.columns[i].name) == 0) {
                            if (table->schema.columns[i].type == COLUMN_TYPE_INT) {
                                int column_value = *((int*)row.columns[i]);
                                int condition_value = atoi(statement.condition_value);
                                if (strcmp(statement.condition_operator, "=") == 0 && column_value != condition_value) {
                                    condition_met = false;
                                } else if (strcmp(statement.condition_operator, "<") == 0 && column_value >= condition_value) {
                                    condition_met = false;
                                } else if (strcmp(statement.condition_operator, ">") == 0 && column_value <= condition_value) {
                                    condition_met = false;
                                }
                                // Add more operators as needed
                            }
                            // Add conditions for other types (DOUBLE, TEXT) as needed
                        }
                    }
                }

                if (condition_met) {
                    gtk_list_store_append(result_store, &tree_iter);
                    for (int i = 0; i < num_columns; i++) {
                        char buffer[256];
                        const char* col_name = statement.num_columns > 0 ? statement.columns[i].name : table->schema.columns[i].name;
                        int col_index = -1;
                        for (int j = 0; j < table->schema.num_columns; j++) {
                            if (strcmp(col_name, table->schema.columns[j].name) == 0) {
                                col_index = j;
                                break;
                            }
                        }
                        if (col_index == -1) {
                            continue;
                        }
                        switch (table->schema.columns[col_index].type) {
                            case COLUMN_TYPE_INT:
                                snprintf(buffer, sizeof(buffer), "%d", *((int *)row.columns[col_index]));
                                break;
                            case COLUMN_TYPE_DOUBLE:
                                snprintf(buffer, sizeof(buffer), "%.2f", *((double *)row.columns[col_index]));
                                break;
                            case COLUMN_TYPE_TEXT:
                                snprintf(buffer, sizeof(buffer), "%s", row.columns[col_index]);
                                break;
                        }
                        gtk_list_store_set(result_store, &tree_iter, i, buffer, -1);
                    }
                }

                cursor_advance(cursor);
            }
            free(cursor);
            gtk_tree_view_set_model(GTK_TREE_VIEW(result_view), GTK_TREE_MODEL(result_store));
        }
    }
    switch (result) {
        case EXECUTE_SUCCESS:
            gtk_text_buffer_insert(output_buffer_widget, &output_iter, "Executed.\n", -1);
            gtk_text_buffer_insert(output_buffer_widget, &output_iter, output_buffer, -1);
            gtk_text_buffer_set_text(buffer, "", -1);
            break;
        case EXECUTE_TABLE_FULL:
            gtk_text_buffer_insert(output_buffer_widget, &output_iter, "Error: Table full.\n", -1);
            gtk_text_buffer_insert(output_buffer_widget, &output_iter, output_buffer, -1);
            break;
        case EXECUTE_DUPLICATE_KEY:
            gtk_text_buffer_insert(output_buffer_widget, &output_iter, "Error: Duplicate key.\n", -1);
            gtk_text_buffer_insert(output_buffer_widget, &output_iter, output_buffer, -1);
            break;
        case EXECUTE_TABLE_NOT_FOUND:
            gtk_text_buffer_insert(output_buffer_widget, &output_iter, "Error: Table not found.\n", -1);
            gtk_text_buffer_insert(output_buffer_widget, &output_iter, output_buffer, -1);
            break;
    }
}


int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Must supply a database filename.\n");
        exit(EXIT_FAILURE);
    }
    char* db_file = argv[1];
    Database db;
    db.table_count = 0;

    load_database(&db, db_file);  // Load the database state

    InputBuffer* input_buffer = new_input_buffer();

    GtkWidget *window;
    GtkWidget *vbox;
    GtkWidget *scroll_window;
    GtkWidget *text_view;
    GtkWidget *execute_button;
    GtkWidget *output_scroll_window;
    GtkWidget *output_text_view;
    GtkWidget *widgets[7]; // 用于传递text_view、table、input_buffer、output_text_view和db_file
    GtkWidget *result_view;
    GtkListStore *result_store;
    GtkTreeViewColumn *col;
    GtkCellRenderer *renderer;
    GtkWidget *scroll_result_window;

    gtk_init(&argc, &argv);

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "XDB");
    gtk_window_set_default_size(GTK_WINDOW(window), 1200, 800); // 调整默认窗体大小
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    // 添加一个滚动窗口容器，用于输入框
    scroll_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll_window, -1, 100); // 设置输入框的高度为100像素
    gtk_box_pack_start(GTK_BOX(vbox), scroll_window, FALSE, FALSE, 0);

    text_view = gtk_text_view_new();
    gtk_container_add(GTK_CONTAINER(scroll_window), text_view);

    execute_button = gtk_button_new_with_label("Execute SQL");
    gtk_box_pack_start(GTK_BOX(vbox), execute_button, FALSE, FALSE, 0);

    // 添加一个滚动窗口容器，用于输出框
    output_scroll_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(output_scroll_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), output_scroll_window, TRUE, TRUE, 0);

    output_text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(output_text_view), FALSE); // 使其不可编辑
    gtk_container_add(GTK_CONTAINER(output_scroll_window), output_text_view);

    // 定义result_view
    scroll_result_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll_result_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), scroll_result_window, TRUE, TRUE, 0);

    result_store = gtk_list_store_new(1, G_TYPE_STRING); // 最初设置为 1 列，稍后会根据查询结果动态更新列数
    result_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(result_store));
    gtk_container_add(GTK_CONTAINER(scroll_result_window), result_view);
    renderer = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("Column 1", renderer, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(result_view), col);

    // 设置字体
    PangoFontDescription *font_desc = pango_font_description_from_string("Monospace 12");
    gtk_widget_override_font(text_view, font_desc);
    gtk_widget_override_font(execute_button, font_desc);
    gtk_widget_override_font(output_text_view, font_desc);
    pango_font_description_free(font_desc);

    // 存储text_view、table和input_buffer
    widgets[0] = text_view;
    widgets[1] = (GtkWidget *)&db;
    widgets[2] = (GtkWidget *)input_buffer;
    widgets[3] = output_text_view;
    widgets[4] = (GtkWidget *)db_file; // 添加db_file
    widgets[5] = result_view;
    widgets[6] = (GtkWidget *)result_store;

    g_signal_connect(execute_button, "clicked", G_CALLBACK(on_execute_button_clicked), widgets);
    // 连接回车键事件
    g_signal_connect(text_view, "key-press-event", G_CALLBACK(on_entry_key_press), NULL);
    gtk_widget_show_all(window);

    gtk_main();

    save_database(&db, db_file);  // Ensure database is saved on normal exit

    return 0;
}