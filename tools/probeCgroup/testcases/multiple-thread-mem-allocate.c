// SPDX-License-Identifier: GPL-2.0
/*
 * multiple-thread-mem-allocate.c - The program to test probeCgroup
 *
 * Copyright (C) Taoxy2004 <221870066@smail.nju.edu.cn>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define MB (1024 * 1024)

void *memory_test(void *)
{
	char *arr[25];
	char *p;
	int i = 0;
	int cnt = 0;

	while (1) {
		for (i = 0; i < 20; i++) {
			p = (char *)malloc(MB);
			memset(p, 0, MB);
			arr[i] = p;
			usleep(10000);
		}
		for (int i = 0; i < 20; i++) {
			free(arr[i]);
			usleep(10000);
		}
	}
}

int main(int argc, char *argv[])
{
	pthread_t threads[4];
	int rc;

	// create threads
	for (int i = 0; i < 4; i++) {
		rc = pthread_create(&threads[i], NULL, memory_test, NULL);
		if (rc != 0) {
			fprintf(stderr, "Error creating thread: %d\n", rc);
			return 1;
		}
	}

	for (int i = 0; i < 4; i++) {
		rc = pthread_join(threads[i], NULL);
		if (rc != 0) {
			fprintf(stderr, "Error joining thread: %d\n", rc);
			return 1;
		}
	}

	return 0;
}
