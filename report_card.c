#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <limits.h>

#define MAX_SUBJECTS 32
#define MAX_STUDENTS 128

typedef struct {
    int student_id;
    double average;
    int minimum;
    int maximum;
    int subject_marks[MAX_SUBJECTS];
} student_stats;

struct thread_params {
    int thread_id;
    int start_index;
    int segment_size;
};

// Global Data accessible to all threads
int num_subjects;
int num_students;
int marks_matrix[MAX_SUBJECTS][MAX_STUDENTS];
student_stats students[MAX_STUDENTS];
student_stats sorted_students[MAX_STUDENTS];
int sort_key;

// ---------------- PHASE 1 ----------------
void* aggregate_worker(void* arg) {
    long student_id = (long)arg; // Using student_id as index
    int sum = 0;
    int min = INT_MAX;
    int max = INT_MIN;

    for (int i = 0; i < num_subjects; i++) {
        int mark = marks_matrix[i][student_id];
        students[student_id].subject_marks[i] = mark;
        sum += mark;
        if (mark < min) min = mark;
        if (mark > max) max = mark;
    }

    students[student_id].student_id = student_id;
    students[student_id].average = (double)sum / num_subjects;
    students[student_id].minimum = min;
    students[student_id].maximum = max;

    pthread_exit(NULL);
}

// ---------------- PHASE 2 ----------------
int compare_students(const void* a, const void* b) {
    student_stats* s1 = (student_stats*)a;
    student_stats* s2 = (student_stats*)b;
    double val1 = 0, val2 = 0;

    if (sort_key == -3) { val1 = s1->maximum; val2 = s2->maximum; }
    else if (sort_key == -2) { val1 = s1->minimum; val2 = s2->minimum; }
    else if (sort_key == -1) { val1 = s1->average; val2 = s2->average; }
    else { val1 = s1->subject_marks[sort_key]; val2 = s2->subject_marks[sort_key]; }

    if (val1 > val2) return -1; // Descending order
    if (val1 < val2) return 1;
    return s1->student_id - s2->student_id; // Tie-breaker: Lower ID first
}

void* sort_worker(void* arg) {
    struct thread_params* tp = (struct thread_params*)arg;
    qsort(&students[tp->start_index], tp->segment_size, sizeof(student_stats), compare_students);
    pthread_exit(NULL);
}

void k_way_merge(int k, struct thread_params* params) {
    int current_indices[k];
    for (int i = 0; i < k; i++) {
        current_indices[i] = params[i].start_index;
    }

    for (int step = 0; step < num_students; step++) {
        int best_segment = -1;
        student_stats best_student;
        
        for (int i = 0; i < k; i++) {
            if (current_indices[i] < params[i].start_index + params[i].segment_size) {
                if (best_segment == -1 || compare_students(&students[current_indices[i]], &best_student) < 0) {
                    best_segment = i;
                    best_student = students[current_indices[i]];
                }
            }
        }
        sorted_students[step] = students[current_indices[best_segment]];
        current_indices[best_segment]++;
    }
}

// ---------------- MAIN LOGIC ----------------
int main() {
    FILE* fp = fopen("input.txt", "r");
    if (!fp) { perror("Failed to open input.txt"); return 1; }

    fscanf(fp, "%d", &num_subjects);
    fscanf(fp, "%d", &num_students);

    for (int i = 0; i < num_subjects; i++) {
        for (int j = 0; j < num_students; j++) {
            fscanf(fp, "%d", &marks_matrix[i][j]);
        }
    }
    fclose(fp);

    // Phase 1: Aggregation
    pthread_t workers[MAX_STUDENTS];
    for (long i = 0; i < num_students; i++) {
        pthread_create(&workers[i], NULL, aggregate_worker, (void*)i);
    }
    for (int i = 0; i < num_students; i++) {
        pthread_join(workers[i], NULL);
    }

    // Phase 2: Sorting
    int k;
    printf("Enter sort key (-3=max, -2=min, -1=avg, 0..%d=subject index): ", num_subjects - 1);
    scanf("%d", &sort_key);
    printf("Enter number of sorting threads (k): ");
    scanf("%d", &k);

    pthread_t sorters[k];
    struct thread_params params[k];
    int segment_size = num_students / k;

    for (int i = 0; i < k; i++) {
        params[i].thread_id = i;
        params[i].start_index = i * segment_size;
        params[i].segment_size = (i == k - 1) ? (segment_size + (num_students % k)) : segment_size;
        pthread_create(&sorters[i], NULL, sort_worker, (void*)&params[i]);
    }
    for (int i = 0; i < k; i++) {
        pthread_join(sorters[i], NULL);
    }
    k_way_merge(k, params);

    // Phase 3: Interactive Menu
    int cmd;
    printf("Report Card Menu\n");
    while (1) {
        printf("Enter command (1 <id> = view student, 2 = view all, 0 = exit): ");
        if (scanf("%d", &cmd) != 1) break;

        if (cmd == 0) break;
        if (cmd == 1) {
            int id;
            scanf("%d", &id);
            if (id >= 0 && id < num_students) {
                printf("Student ID\t: %d\nAverage\t\t: %.2f\nMinimum\t\t: %d\nMaximum\t\t: %d\nSubject Marks: [", 
                       students[id].student_id, students[id].average, students[id].minimum, students[id].maximum);
                for (int m = 0; m < num_subjects; m++) {
                    printf("%d%s", students[id].subject_marks[m], (m == num_subjects - 1) ? "" : ", ");
                }
                printf("]\n");
            }
        } else if (cmd == 2) {
            for (int i = 0; i < num_students; i++) {
                double val = 0;
                if (sort_key == -3) val = sorted_students[i].maximum;
                else if (sort_key == -2) val = sorted_students[i].minimum;
                else if (sort_key == -1) val = sorted_students[i].average;
                else val = sorted_students[i].subject_marks[sort_key];
                printf("s%d\t%.2f\n", sorted_students[i].student_id, val);
            }
        }
    }
    return 0;
}
