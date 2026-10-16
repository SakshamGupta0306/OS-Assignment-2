// Inside kernel_clone()
extern int check_fork_storm(void); // Declared in your custom header
int contain_status = check_fork_storm();
if (contain_status < 0) {
    return contain_status; // Returns -EAGAIN for Stage 1 THROTTLE
}
