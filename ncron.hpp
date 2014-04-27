#ifndef NCRON_NCRON_HPP_
#define NCRON_NCRON_HPP_

#define MAXLINE 1024
#define MAX_BUF 1024

void clock_or_die(struct timespec *ts);
void show_usage(void);
void print_version(void);

extern int g_initial_sleep;
extern char g_ncron_conf[PATH_MAX];
extern char g_ncron_execfile[PATH_MAX];
extern char g_ncron_pidfile[PATH_MAX];
extern int g_ncron_execmode;

#endif /* NCRON_NCRON_HPP_ */
