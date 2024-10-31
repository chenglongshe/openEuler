// SPDX-License-Identifier: GPL-2.0
/*
 * mem-allocate.c - The program to test probeCgroup
 *
 * Copyright (C) Taoxy2004 <221870066@smail.nju.edu.cn>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MB (1024 * 1024)

int main(int argc, char *argv[])
{
	char *p;
	int i = 0;

	while (1) {
		p = (char *)malloc(MB);
		memset(p, 0, MB);
		sleep(1);
	}

	return 0;
}
