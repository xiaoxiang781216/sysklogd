/*-
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ksym_mod.c - functions for building symbol lookup tables for klogd
 * Copyright (c) 1995, 1996  Dr. G.W. Wettstein <greg@wind.rmcc.com>
 * Copyright (c) 1996 Enjellic Systems Development
 * Copyright (c) 1998-2007 Martin Schulze <joey@infodrom.org>
 *
 * This file is part of the sysklogd package, a kernel and system log daemon.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this file; see the file COPYING.  If not, write to the
 * Free Software Foundation, 51 Franklin Street - Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

/*
 * This file implements functions which are useful for building
 * a symbol lookup table based on the in kernel symbol table
 * maintained by the Linux kernel.
 *
 * Proper logging of kernel panics generated by loadable modules
 * tends to be difficult.  Since the modules are loaded dynamically
 * their addresses are not known at kernel load time.  A general
 * protection fault (Oops) cannot be properly deciphered with 
 * classic methods using the static symbol map produced at link time.
 *
 * One solution to this problem is to have klogd attempt to translate
 * addresses from module when the fault occurs.  By referencing the
 * the kernel symbol table proper resolution of these symbols is made
 * possible.
 *
 * At least that is the plan.
 */

#include "config.h"
#include <ctype.h>
#include <errno.h>
#include <malloc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include "module.h"
#include <linux/version.h>
#include <paths.h>
#include <stdarg.h>
#include <sys/stat.h>

#include "klogd.h"
#include "ksyms.h"

#define KSYMS "/proc/kallsyms"

static int     num_modules = 0;
struct Module *sym_array_modules = NULL;

static int have_modules = 0;

#if defined(TEST)
static int debugging = 1;
#else
extern int debugging;
#endif

/* Function prototypes. */
static void    FreeModules(void);
static int     AddSymbol(const char *);
struct Module *AddModule(const char *);
static int     symsort(const void *, const void *);

/* Imported from ksym.c */
extern int num_syms;

/**************************************************************************
 * Function:	InitMsyms
 *
 * Purpose:	This function is responsible for building a symbol
 *		table which can be used to resolve addresses for
 *		loadable modules.
 *
 * Arguments:	Void
 *
 * Return:	A boolean return value is assumed.
 *
 *		A false value indicates that something went wrong.
 *
 *		True if loading is successful.
 **************************************************************************/

extern int InitMsyms(void)
{
	FILE *ksyms;
	char *p;
	char buf[128];
	int rtn;
	int tmp;

	/* Initialize the kernel module symbol table. */
	FreeModules();

	ksyms = fopen(KSYMS, "r");

	if (ksyms == NULL) {
		if (errno == ENOENT)
			Syslog(LOG_INFO, "No module symbols loaded - "
			                 "kernel modules not enabled.\n");
		else
			Syslog(LOG_ERR, "Error loading kernel symbols "
			                "- %s\n",
			       strerror(errno));
		return 0;
	}

	if (debugging)
		fprintf(stderr, "Loading kernel module symbols - "
		                "Source: %s\n",
		        KSYMS);

	while (fgets(buf, sizeof(buf), ksyms) != NULL) {
		if (num_syms > 0 && index(buf, '[') == NULL)
			continue;

		p = index(buf, ' ');

		if (p == NULL)
			continue;

		if (buf[strlen(buf) - 1] == '\n')
			buf[strlen(buf) - 1] = '\0';
		/* overlong lines will be ignored above */

		AddSymbol(buf);
	}

	fclose(ksyms);

	have_modules = 1;

	/* Sort the symbol tables in each module. */
	for (rtn = tmp = 0; tmp < num_modules; ++tmp) {
		rtn += sym_array_modules[tmp].num_syms;
		if (sym_array_modules[tmp].num_syms < 2)
			continue;
		qsort(sym_array_modules[tmp].sym_array,
		      sym_array_modules[tmp].num_syms,
		      sizeof(struct sym_table), symsort);
	}

	if (rtn == 0)
		Syslog(LOG_INFO, "No module symbols loaded.");
	else
		Syslog(LOG_INFO, "Loaded %d %s from %d module%s", rtn,
		       (rtn == 1) ? "symbol" : "symbols",
		       num_modules, (num_modules == 1) ? "." : "s.");

	return 1;
}

static int symsort(const void *p1, const void *p2)
{
	const struct sym_table *sym1 = p1,
			       *sym2 = p2;

	if (sym1->value < sym2->value)
		return -1;

	if (sym1->value == sym2->value)
		return 0;

	return 1;
}

/**************************************************************************
 * Function:	FreeModules
 *
 * Purpose:	This function is used to free all memory which has been
 *		allocated for the modules and their symbols.
 *
 * Arguments:	None specified.
 *
 * Return:	void
 **************************************************************************/
static void FreeModules(void)
{
	struct Module *mp;
	int nmods;
	int nsyms;

	/* Check to see if the module symbol tables need to be cleared. */
	have_modules = 0;
	if (num_modules == 0)
		return;

	if (sym_array_modules == NULL)
		return;

	for (nmods = 0; nmods < num_modules; ++nmods) {
		mp = &sym_array_modules[nmods];
		if (mp->num_syms == 0)
			continue;

		for (nsyms = 0; nsyms < mp->num_syms; ++nsyms)
			free(mp->sym_array[nsyms].name);
		free(mp->sym_array);
		if (mp->name != NULL)
			free(mp->name);
	}

	free(sym_array_modules);
	sym_array_modules = NULL;
	num_modules = 0;
}

/**************************************************************************
 * Function:	AddModule
 *
 * Purpose:	This function is responsible for adding a module to
 *		the list of currently loaded modules.
 *
 * Arguments:	(const char *) module
 *
 *		module:->	The name of the module.
 *
 * Return:	struct Module *
 **************************************************************************/

struct Module *AddModule(const char *module)
{
	struct Module *mp;

	if (num_modules == 0) {
		sym_array_modules = (struct Module *)malloc(sizeof(struct Module));
		if (sym_array_modules == NULL) {
			Syslog(LOG_WARNING, "Cannot allocate Module array.\n");
			return NULL;
		}
		mp = sym_array_modules;
	} else {
		/* Allocate space for the module. */
		mp = realloc(sym_array_modules, (num_modules + 1) * sizeof(struct Module));
		if (mp == NULL) {
			Syslog(LOG_WARNING, "Cannot allocate Module array.\n");
			return NULL;
		}

		sym_array_modules = mp;
		mp = &sym_array_modules[num_modules];
	}

	num_modules++;
	mp->sym_array = NULL;
	mp->num_syms = 0;

	if (module != NULL)
		mp->name = strdup(module);
	else
		mp->name = NULL;

	return mp;
}

/**************************************************************************
 * Function:	AddSymbol
 *
 * Purpose:	This function is responsible for adding a symbol name
 *		and its address to the symbol table.
 *
 * Arguments:	(struct Module *) mp, (unsigned long) address, (char *) symbol
 *
 *		mp:->	A pointer to the module which the symbol is
 *			to be added to.
 *
 *		address:->	The address of the symbol.
 *
 *		symbol:->	The name of the symbol.
 *
 * Return:	int
 *
 *		A boolean value is assumed.  True if the addition is
 *		successful.  False if not.
 **************************************************************************/
static int AddSymbol(const char *line)
{
	static char *lastmodule = NULL;
	struct Module *mp;
	unsigned long address;
	char *module;
	char *p;

	module = index(line, '[');
	if (module != NULL) {
		p = index(module, ']');

		if (p != NULL)
			*p = '\0';

		p = module++;

		while (isspace(*(--p)))
			;
		*(++p) = '\0';
	}

	p = index(line, ' ');
	if (p == NULL)
		return 0;
	*p = '\0';

	address = strtoul(line, NULL, 16);

	p += 3;

	if (num_modules == 0 ||
	    (lastmodule == NULL && module != NULL) ||
	    (module == NULL && lastmodule != NULL) ||
	    (module != NULL && strcmp(module, lastmodule))) {
		mp = AddModule(module);

		if (mp == NULL)
			return 0;
	} else
		mp = &sym_array_modules[num_modules - 1];

	lastmodule = mp->name;

	/* Allocate space for the symbol table entry. */
	mp->sym_array = (struct sym_table *)realloc(mp->sym_array,
	                                            (mp->num_syms + 1) * sizeof(struct sym_table));

	if (mp->sym_array == NULL)
		return 0;

	mp->sym_array[mp->num_syms].name = strdup(p);
	if (mp->sym_array[mp->num_syms].name == NULL)
		return 0;

	/* Stuff interesting information into the module. */
	mp->sym_array[mp->num_syms].value = address;
	++mp->num_syms;

	return 1;
}

/**************************************************************************
 * Function:	LookupModuleSymbol
 *
 * Purpose:	Find the symbol which is related to the given address from
 *		a kernel module.
 *
 * Arguments:	(long int) value, (struct symbol *) sym
 *
 *		value:->	The address to be located.
 * 
 *		sym:->		A pointer to a structure which will be
 *				loaded with the symbol's parameters.
 *
 * Return:	(char *)
 *
 *		If a match cannot be found a diagnostic string is printed.
 *		If a match is found the pointer to the symbolic name most
 *		closely matching the address is returned.
 **************************************************************************/
char *LookupModuleSymbol(unsigned long value, struct symbol *sym)
{
	static char ret[100];
	struct sym_table *last;
	struct Module *mp;
	int nmod;
	int nsym;

	sym->size = 0;
	sym->offset = 0;
	if (num_modules == 0)
		return NULL;

	for (nmod = 0; nmod < num_modules; ++nmod) {
		mp = &sym_array_modules[nmod];

		/*
		 * Run through the list of symbols in this module and
		 * see if the address can be resolved.
		 */
		for (nsym = 1, last = &mp->sym_array[0];
		     nsym < mp->num_syms;
		     ++nsym) {
			if (mp->sym_array[nsym].value > value) {
				if (sym->size == 0 ||
				    (value - last->value) < (unsigned long)sym->offset ||
				    (((unsigned long)sym->offset == (value - last->value)) &&
				     (mp->sym_array[nsym].value - last->value) < (unsigned long)sym->size)) {
					sym->offset = value - last->value;
					sym->size = mp->sym_array[nsym].value -
					            last->value;
					ret[sizeof(ret) - 1] = '\0';
					if (mp->name == NULL)
						snprintf(ret, sizeof(ret) - 1,
						         "%s", last->name);
					else
						snprintf(ret, sizeof(ret) - 1,
						         "%s:%s", mp->name, last->name);
				}
				break;
			}
			last = &mp->sym_array[nsym];
		}
	}

	if (sym->size > 0)
		return ret;

	/* It has been a hopeless exercise. */
	return NULL;
}

/*
 * Setting the -DTEST define enables the following code fragment to
 * be compiled.  This produces a small standalone program which will
 * dump the current kernel symbol table.
 */
#if defined(TEST)

#include <stdarg.h>

int main(int argc, char *argv[])
{
	int lp, syms;

	if (!InitMsyms()) {
		fprintf(stderr, "Cannot load module symbols.\n");
		return 1;
	}

	printf("Number of modules: %d\n\n", num_modules);

	for (lp = 0; lp < num_modules; ++lp) {
		printf("Module #%d = %s, Number of symbols = %d\n", lp + 1,
		       sym_array_modules[lp].name == NULL
		           ? "kernel space"
		           : sym_array_modules[lp].name,
		       sym_array_modules[lp].num_syms);

		for (syms = 0; syms < sym_array_modules[lp].num_syms; ++syms) {
			printf("\tSymbol #%d\n", syms + 1);
			printf("\tName: %s\n",
			       sym_array_modules[lp].sym_array[syms].name);
			printf("\tAddress: %lx\n\n",
			       sym_array_modules[lp].sym_array[syms].value);
		}
	}

	FreeModules();
	return 0;
}

void Syslog(int priority, char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stdout, "Pr: %d, ", priority);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
	fputc('\n', stdout);
}

#endif /* TEST */

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
