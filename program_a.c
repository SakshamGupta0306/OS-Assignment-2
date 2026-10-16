#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <time.h>
#include "workload_cpu.h" // CPU-bound header

#define NUM_CHILDREN 8

struct child_stats {
    int id;
    double start_ms;
    double end_ms;
};

// Helper function to calculate time difference in milliseconds
double get_time_ms(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1000000.0;
}

int main() {
    struct timespec ref_time;
    // Parent records reference timestamp before forking
    clock_gettime(CLOCK_MONOTONIC, &ref_time);

    int pipefd[2];
    if (pipe(pipefd) == -1) { 
        perror("pipe"); 
        exit(1); 
    }

    for (int i = 0; i < NUM_CHILDREN; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            exit(1);
        }
        
        if (pid == 0) { // Child Process
            close(pipefd[0]); // Close read end of the pipe
            
            struct timespec start_time, end_time;
            clock_gettime(CLOCK_MONOTONIC, &start_time); // Start timestamp

            cpu_task(i); // Execute CPU-bound workload

            clock_gettime(CLOCK_MONOTONIC, &end_time); // Completion timestamp

            // Package stats to send to parent
            struct child_stats stats;
            stats.id = i;
            stats.start_ms = get_time_ms(ref_time, start_time);
            stats.end_ms = get_time_ms(ref_time, end_time);

            // Send to parent and exit
            if (write(pipefd[1], &stats, sizeof(stats)) != sizeof(stats)) {
                perror("write to pipe");
            }
            close(pipefd[1]);
            exit(0);
        }
    }

    close(pipefd[1]); // Parent closes write end

    // Wait for all children to complete
    for (int i = 0; i < NUM_CHILDREN; i++) {
        wait(NULL);
    }

    double max_response = 0, max_turnaround = 0;
    double total_response = 0, total_turnaround = 0;
    double last_end = 0;

    // Read all 8 stats from the pipe
    for (int i = 0; i < NUM_CHILDREN; i++) {
        struct child_stats stats;
        if (read(pipefd[0], &stats, sizeof(stats)) == sizeof(stats)) {
            total_response += stats.start_ms;
            total_turnaround += stats.end_ms;

            if (stats.start_ms > max_response) max_response = stats.start_ms;
            if (stats.end_ms > max_turnaround) max_turnaround = stats.end_ms;
            if (stats.end_ms > last_end) last_end = stats.end_ms;
        }
    }
    close(pipefd[0]);

    // Retrieve aggregated context switch information for children
    struct rusage usage;
    getrusage(RUSAGE_CHILDREN, &usage);

    // Calculate averages and throughput
    double avg_response = total_response / NUM_CHILDREN;
    double avg_turnaround = total_turnaround / NUM_CHILDREN;
    double total_elapsed_sec = last_end / 1000.0;
    double throughput = NUM_CHILDREN / total_elapsed_sec;

    // Print output matching the required sample format
    printf("Involuntary Context Switches: %ld\n", usage.ru_nivcsw);
    printf("Voluntary Context Switches: %ld\n", usage.ru_nvcsw);
    printf("Average Response Time: %.2fms\n", avg_response);
    printf("Maximum Response Time: %.2fms\n", max_response);
    printf("Average Turnaround Time: %.2fms\n", avg_turnaround);
    printf("Maximum Turnaround Time: %.2fms\n", max_turnaround);
    printf("Throughput: %.2f children/s\n", throughput);
    printf("Total Elapsed Time: %.2fs\n", total_elapsed_sec);

    return 0;
}

/*
gcc -Wall -Wextra -O2 -o program_a program_a.c
gcc -Wall -Wextra -O2 -o program_b program_b.c

sudo sysctl -w kernel.sched_rr_timeslice_ms=10

sudo taskset -c 0 chrt -r 20 ./program_a

sudo timeout 30 taskset -c 0 chrt -f 20 ./program_a
sudo timeout 30 taskset -c 0 chrt -f 20 ./program_b
*/
