#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

struct unit_params {
    int unit_id;
};

struct unit_result {
    int unit_id;
    int valid;
    int missing_mask;
    int dup_mask;
    int blank_count;
};

struct block_params {
    int block_id;
};

// Global Arrays for Data Partitioning (Thread-Safe without Mutexes)
int grid[9][9];
int validity[27];
int global_dup_mask[27];
int heatmap[9][9];

// ---------------- PHASE 1: VALIDATION ----------------
void* validate_worker(void* arg) {
    struct unit_params* params = (struct unit_params*)arg;
    int id = params->unit_id;
    
    struct unit_result* result = malloc(sizeof(struct unit_result));
    result->unit_id = id;
    result->blank_count = 0;
    result->missing_mask = 0;
    result->dup_mask = 0;
    
    int counts[10] = {0}; // Index 1-9 for digits

    // Extract the 9 cells based on unit type
    for (int i = 0; i < 9; i++) {
        int val = 0;
        if (id < 9) {
            val = grid[id][i]; // Row
        } else if (id < 18) {
            val = grid[i][id - 9]; // Column
        } else {
            int block = id - 18;
            int r = (block / 3) * 3 + (i / 3);
            int c = (block % 3) * 3 + (i % 3);
            val = grid[r][c]; // Sub-grid
        }

        if (val == 0) {
            result->blank_count++;
        } else {
            counts[val]++;
        }
    }

    // Populate masks
    int is_valid = (result->blank_count == 0) ? 1 : 0;
    for (int d = 1; d <= 9; d++) {
        if (counts[d] == 0) {
            result->missing_mask |= (1 << d);
            is_valid = 0;
        } else if (counts[d] > 1) {
            result->dup_mask |= (1 << d);
            is_valid = 0;
        }
    }

    result->valid = is_valid;
    validity[id] = is_valid; // Safe: Each thread writes to its own index

    pthread_exit((void*)result);
}

// ---------------- PHASE 2: ANALYTICS ----------------
void* analytics_worker(void* arg) {
    struct block_params* params = (struct block_params*)arg;
    int block_id = params->block_id;
    
    int start_r = (block_id / 3) * 3;
    int start_c = (block_id % 3) * 3;

    // Analyze only the 9 cells belonging to this thread's block
    for (int i = 0; i < 9; i++) {
        int r = start_r + (i / 3);
        int c = start_c + (i % 3);
        int val = grid[r][c];

        if (val == 0) {
            heatmap[r][c] = 3;
        } else {
            int score = 0;
            // Check row (0-8), column (9-17), and subgrid (18-26) masks
            if (global_dup_mask[r] & (1 << val)) score++;
            if (global_dup_mask[9 + c] & (1 << val)) score++;
            if (global_dup_mask[18 + block_id] & (1 << val)) score++;
            
            heatmap[r][c] = score;
        }
    }
    pthread_exit(NULL);
}

// ---------------- MAIN LOGIC ----------------
int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <grid_file>\n", argv[0]);
        return 1;
    }

    FILE* fp = fopen(argv[1], "r");
    if (!fp) { perror("Failed to open file"); return 1; }

    int count = 0;
    char ch;
    // Read exactly 81 valid characters ('1'-'9', '0', or '.')
    while ((ch = fgetc(fp)) != EOF && count < 81) {
        if (ch >= '1' && ch <= '9') {
            grid[count / 9][count % 9] = ch - '0';
            count++;
        } else if (ch == '0' || ch == '.') {
            grid[count / 9][count % 9] = 0;
            count++;
        }
    }
    fclose(fp);

    if (count != 81) {
        fprintf(stderr, "Error: Invalid grid. Expected 81 cells.\n");
        return 1;
    }

    printf("Input Grid:\n");
    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) {
            printf("%d ", grid[r][c]);
        }
        printf("\n");
    }
    printf("\n");

    // Phase 1: Validation
    pthread_t val_threads[27];
    struct unit_params v_params[27];
    struct unit_result* results[27];

    for (int i = 0; i < 27; i++) {
        v_params[i].unit_id = i;
        pthread_create(&val_threads[i], NULL, validate_worker, &v_params[i]);
    }

    int valid_count = 0;
    for (int i = 0; i < 27; i++) {
        pthread_join(val_threads[i], (void**)&results[i]);
        if (results[i]->valid) valid_count++;
        
        // Save duplicate masks for Phase 2 globally
        global_dup_mask[i] = results[i]->dup_mask; 
    }

    if (valid_count == 27) {
        printf("VALID\n");
        for (int i = 0; i < 27; i++) free(results[i]);
        return 0;
    }

    // Phase 2: Analytics
    printf("INVALID\n");
    printf("Validity Score: %.2f%%\n\n", (valid_count / 27.0) * 100.0);

    pthread_t heat_threads[9];
    struct block_params h_params[9];

    for (int i = 0; i < 9; i++) {
        h_params[i].block_id = i;
        pthread_create(&heat_threads[i], NULL, analytics_worker, &h_params[i]);
    }

    for (int i = 0; i < 9; i++) {
        pthread_join(heat_threads[i], NULL);
    }

    printf("Conflict Density Heatmap:\n");
    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) {
            printf("%d ", heatmap[r][c]);
        }
        printf("\n");
    }
    printf("\n");

    printf("Hotspots (score 3):\n");
    int hotspot_count = 0;
    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) {
            if (heatmap[r][c] == 3) {
                printf("(%d,%d) value=%d\n", r, c, grid[r][c]);
                hotspot_count++;
            }
        }
    }
    printf("Total Hotspots: %d\n\n", hotspot_count);

    printf("Missing Digit Report:\n");
    for (int i = 0; i < 27; i++) {
        if (!results[i]->valid) {
            char* type = (i < 9) ? "Row" : (i < 18) ? "Column" : "Sub-grid";
            int idx = (i < 9) ? i : (i < 18) ? (i - 9) : (i - 18);
            
            printf("%-8s %d: missing (", type, idx);
            int first = 1;
            for (int d = 1; d <= 9; d++) {
                if (results[i]->missing_mask & (1 << d)) {
                    if (!first) printf(" ");
                    printf("%d", d);
                    first = 0;
                }
            }
            printf(") blanks=%d\n", results[i]->blank_count);
        }
        free(results[i]); // Clean up memory properly
    }

    return 0;
}

/* 
gcc -Wall -Wextra -O2 -pthread -o sudoku_analyzer sudoku_analyzer.c

valgrind --leak-check=full ./sudoku_analyzer grid_invalid.txt
*/
