#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <X11/Xlib.h>
#include <sys/signalfd.h>
#include <poll.h>
#define LENGTH(X) (sizeof(X) / sizeof (X[0]))
#define MAX_CMD_OUTPUT_LENGTH 50
#define MAX_STATUS_LENGTH 256

typedef struct {
	char* icon;
	char* command;
	unsigned int interval;
	unsigned int signal;
} Block;
void sighandler();
void buttonhandler(int ssi_int);
void replace(char *str, char old, char new);
void remove_all(char *str, char to_remove);
void getcmds(int time);
void getsigcmds(int signal);
void setupsignals();
int getstatus(char *str, char *last);
void setroot();
void statusloop();
int calculateUpdateInterval();
void termHandler(int signum);

static char statusMessages[LENGTH(blocks)][MAX_CMD_OUTPUT_LENGTH] = { 0 };
static char statusBarOutput[MAX_STATUS_LENGTH];
static char oldStatusBarOutput[MAX_STATUS_LENGTH];
static int statusContinue = 1;
static int signalFD;
static int timerInterval = -1;
static void (*writestatus) () = setroot;

void replace(char *str, char old, char new) {
	for (char * c = str; *c; c++)
		if (*c == old)
			*c = new;
}

void removeAll(char *str, char target) {
	char *read = str;
	char *write = str;

	while (*read) {
		if (*read != target) {
			*write++ = *read;
		}
		++read;
	}
	*write = '\0';
}

int gcd(int a, int b)
{
	int temp;
	while (b > 0) {
		temp = a % b;

		a = b;
		b = temp;
	}
	return a;
}

void setStatusBlock(const Block *block, char *output, char* cmdOutput) {
	int statusLength = strlen(block->icon);
	strcpy(output, block->icon);
	strcpy(output + statusLength, cmdOutput);
	removeAll(output, '\n');
	statusLength = strlen(output);

	int isNotLastBlock = block != &blocks[LENGTH(blocks) - 1];
	int shouldAddDelim = statusLength > 0 && isNotLastBlock;
	if (shouldAddDelim) {
		strcat(output, delim);
		statusLength += strlen(delim);
	}
	output[statusLength++] = '\0';
}

char* runCommand(const Block *block, char *cmdOutput) {
	char *cmd = block->command;
	FILE *cmdFd = popen(cmd, "r");

	if (!cmdFd) {
		return NULL;
	}

	// TODO: Decide whether its better to use the last value till next time or just keep trying while the error was the interrupt
	// this keeps trying to read if it got nothing and the error was an interrupt
	// could also just read to a separate buffer and not move the data over if interrupted
	// this way will take longer trying to complete 1 thing but will get it done
	// the other way will move on to keep going with everything and the part that failed to read will be wrong till its updated again
	// either way you have to save the data to a temp buffer because when it fails it writes nothing and then then it gets displayed before this finishes
	char * s;
	int e;

	do {
		errno = 0;
		s = fgets(cmdOutput, MAX_CMD_OUTPUT_LENGTH - (strlen(delim) + 1), cmdFd);
		e = errno;
	} while (!s && e == EINTR);
	pclose(cmdFd);

	return cmdOutput;
}

void updateStatusMessage(const Block *block, char *output)
{
	if (block->signal != 0) {
		output[0] = block->signal;
		output++;
	}

	char cmdOutput[MAX_CMD_OUTPUT_LENGTH] = "";
	runCommand(block, cmdOutput);

	setStatusBlock(block, output, cmdOutput);
}

void updateStatusMessages(int time)
{
	const Block* block;

	for (int i = 0; i < LENGTH(blocks); i++) {
		block = blocks + i;

		int forceUpdate = time == -1;
		int shouldUpdate = block->interval != 0 && time % block->interval == 0;
		if (forceUpdate || shouldUpdate) {
			updateStatusMessage(block, statusMessages[i]);
		}
	}
}

void getsigcmds(int signal)
{
	const Block *current;
	for (int i = 0; i < LENGTH(blocks); i++) {
		current = blocks + i;
		if (current->signal == signal) {
			updateStatusMessage(current, statusMessages[i]);
		}
	}
}

void setupSignals()
{
	sigset_t signals;
	sigemptyset(&signals);
	sigaddset(&signals, SIGALRM); // Timer events
	sigaddset(&signals, SIGUSR1); // Button events
	// All signals assigned to blocks
	for (size_t i = 0; i < LENGTH(blocks); i++)
		if (blocks[i].signal > 0)
			sigaddset(&signals, SIGRTMIN + blocks[i].signal);
	// Create signal file descriptor for pooling
	signalFD = signalfd(-1, &signals, 0);
	// Block all real-time signals
	for (int i = SIGRTMIN; i <= SIGRTMAX; i++) sigaddset(&signals, i);
	sigprocmask(SIG_BLOCK, &signals, NULL);
	// Do not transform children into zombies
	struct sigaction sigchld_action = {
			.sa_handler = SIG_DFL,
			.sa_flags = SA_NOCLDWAIT
	};
	sigaction(SIGCHLD, &sigchld_action, NULL);
}

// This function can be splitted into multiple functions, but
// I've added comments for the sake of simplicity.
int updateStatusBarOutput()
{
	// Cache previous output
	strcpy(oldStatusBarOutput, statusBarOutput);

	// Update status bar output
	statusBarOutput[0] = '\0';
	for (int i = 0; i < LENGTH(blocks); i++) {
		strcat(statusBarOutput, statusMessages[i]);
		if (i == LENGTH(blocks) - 1)
			strcat(statusBarOutput, " ");
	}
	statusBarOutput[strlen(statusBarOutput)-1] = '\0';

	// Check if it's been updated
	return strcmp(statusBarOutput, oldStatusBarOutput);
}

void updateWindowName()
{
	if (!updateStatusBarOutput())
		return;

	Display *display = XOpenDisplay(NULL);
	int screen = DefaultScreen(display);
	Window root = RootWindow(display, screen);

	XStoreName(display, root, statusBarOutput);

	XCloseDisplay(display);
}

void pstdout()
{
	if (!updateStatusBarOutput())
		return;
	printf("%s\n", statusBarOutput);
	fflush(stdout);
}

void updateStatusIndefinitely()
{
	setupsignals();
	// first figure out the default wait interval by finding the
	// greatest common denominator of the intervals
	for(int i = 0; i < LENGTH(blocks); i++){
			if(blocks[i].interval){
					timerInterval = gcd(blocks[i].interval, timerInterval);
			}
	}
	getcmds(-1);     // Fist time run all commands
	raise(SIGALRM);  // Schedule first timer event
	int ret;
	struct pollfd pfd[] = {{.fd = signalFD, .events = POLLIN}};
	while (statusContinue) {
			// Wait for new signal
			ret = poll(pfd, sizeof(pfd) / sizeof(pfd[0]), -1);
			if (ret < 0 || !(pfd[0].revents & POLLIN)) break;
			sighandler(); // Handle signal
	}
}

void sighandler()
{
	static int time = 0;
	struct signalfd_siginfo si;
	int ret = read(signalFD, &si, sizeof(si));
	if (ret < 0) return;
	int signal = si.ssi_signo;
	switch (signal) {
		case SIGALRM:
			// Execute blocks and schedule the next timer event
			getcmds(time);
			alarm(timerInterval);
			time += timerInterval;
			break;
		case SIGUSR1:
			// Handle buttons
			buttonhandler(si.ssi_int);
			return;
		default:
			// Execute the block that has the given signal
			getsigcmds(signal - SIGRTMIN);
			break;
	}
	writestatus();
}

void buttonhandler(int ssi_int)
{
	char button[2] = {'0' + ssi_int & 0xff, '\0'};
	pid_t process_id = getpid();
	int sig = ssi_int >> 8;
	if (fork() == 0) {
		const Block *current;
		for (int i = 0; i < LENGTH(blocks); i++) {
			current = blocks + i;
			if (current->signal == sig)
				break;
		}
		char shcmd[1024];
		sprintf(shcmd,"%s && kill -%d %d",current->command, current->signal+34,process_id);
		char *command[] = { "/bin/sh", "-c", shcmd, NULL };
		setenv("BLOCK_BUTTON", button, 1);
		setsid();
		execvp(command[0], command);
		exit(EXIT_SUCCESS);
	}

	const Block *current;
	for (int i = 0; i < LENGTH(blocks); i++) {
		current = blocks + i;
		if (current->signal == sig)
			break;
	}

	char shCmd[1024];
	sprintf(shCmd, "%s && kill -%d %d", current->command, current->signal+34, process_id);

	char *command[] = { "/bin/sh", "-c", shCmd, NULL };
	setenv("BLOCK_BUTTON", button, 1);
	setsid();
	execvp(command[0], command);
	exit(EXIT_SUCCESS);
}

void termHandler(int signum)
{
	statusContinue = 0;
}

int main(int argc, char** argv)
{
	for (int i = 0; i < argc; i++) {
		if (!strcmp("-d", argv[i]))
			delim = argv[++i];
		else if (!strcmp("-p", argv[i]))
			writeStatus = pstdout;
	}

	signal(SIGTERM, termHandler);
	signal(SIGINT, termHandler);

	statusloop();
	close(signalFD);
}
