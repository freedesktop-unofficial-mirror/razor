/*
 * Copyright (C) 2008  Kristian Høgsberg <krh@redhat.com>
 * Copyright (C) 2008  Red Hat, Inc
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <fnmatch.h>
#include <assert.h>

#include "razor-internal.h"
#include "razor.h"

void *
zalloc(size_t size)
{
	void *p;

	p = malloc(size);
	memset(p, 0, size);

	return p;
}

struct razor_set_section_index {
	const char *name;
	uint32_t offset;
	uint32_t flags;
};

#define MAIN(type, field) \
	{ type, offsetof(struct razor_set, field), RAZOR_SECTION_MAIN }
#define FILES(type, field) \
	{ type, offsetof(struct razor_set, field), RAZOR_SECTION_FILES }
#define DETAILS(type, field) \
	{ type, offsetof(struct razor_set, field), RAZOR_SECTION_DETAILS }

struct razor_set_section_index razor_sections[] = {
	MAIN(RAZOR_STRING_POOL, string_pool),
	MAIN(RAZOR_PACKAGES, packages),
	MAIN(RAZOR_PROPERTIES, properties),
	MAIN(RAZOR_PACKAGE_POOL, package_pool),
	MAIN(RAZOR_PROPERTY_POOL, property_pool),
	FILES(RAZOR_FILES, files),
	FILES(RAZOR_FILE_POOL, file_pool),
	FILES(RAZOR_FILE_STRING_POOL, file_string_pool),
	DETAILS(RAZOR_DETAILS_STRING_POOL, details_string_pool)
};

RAZOR_EXPORT struct razor_set *
razor_set_create(void)
{
	struct razor_set *set;
	struct razor_entry *e;
	char *empty;

	set = zalloc(sizeof *set);

	e = array_add(&set->files, sizeof *e);
	empty = array_add(&set->string_pool, 1);
	*empty = '\0';
	e->name = 0;
	e->flags = RAZOR_ENTRY_LAST;
	e->start = 0;
	list_set_empty(&e->packages);

	return set;
}

struct razor_mapped_file {
	struct razor_set_header *header;
	size_t size;
	struct razor_mapped_file *next;
};

RAZOR_EXPORT int
razor_set_bind_sections(struct razor_set *set, const char *filename)
{
	struct razor_set_section *s, *sections;
	struct razor_mapped_file *file;
	struct stat stat;
	const char *pool;
	struct array *array;
	int fd, i, j;

	file = zalloc(sizeof *file);
	if (file == NULL)
		return -1;

	fd = open(filename, O_RDONLY);
	if (fd < 0 || fstat(fd, &stat) < 0) {
		free(file);
		return -1;
	}

	file->header = mmap(NULL, stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (file->header == MAP_FAILED) {
		free(file);
		return -1;
	}

	file->size = stat.st_size;
	file->next = set->mapped_files;
	set->mapped_files = file;

	sections = (void *) file->header + sizeof *file->header;
	pool = (void *) sections +
		file->header->num_sections * sizeof *sections;

	for (i = 0; i < file->header->num_sections; i++) {
		s = sections + i;
		for (j = 0; j < ARRAY_SIZE(razor_sections); j++)
			if (!strcmp(razor_sections[j].name, &pool[s->name]))
				break;
		if (j == ARRAY_SIZE(razor_sections))
			continue;
		array = (void *) set + razor_sections[j].offset;
		array->data = (void *) file->header + s->offset;
		array->size = s->size;
		array->alloc = s->size;
	}

	return 0;
}

RAZOR_EXPORT struct razor_set *
razor_set_open(const char *filename)
{
	struct razor_set *set;

	set = zalloc(sizeof *set);
	if (razor_set_bind_sections(set, filename)) {
		free(set);
		return NULL;
	}
	return set;
}

RAZOR_EXPORT void
razor_set_destroy(struct razor_set *set)
{
	struct razor_mapped_file *file, *next;
	struct array *array;
	int i;

	assert (set != NULL);

	if (set->mapped_files == NULL) {
		for (i = 0; i < ARRAY_SIZE(razor_sections); i++) {
			array = (void *) set + razor_sections[i].offset;
			array_release(array);
		}
	} else {
		for (file = set->mapped_files; file != NULL; file = next) {
			next = file->next;
			munmap(file->header, file->size);
			free(file);
		}
	}

	free(set);
}

RAZOR_EXPORT int
razor_set_write_to_fd(struct razor_set *set, int fd, uint32_t section_mask)
{
	struct razor_set_header header;
	struct razor_set_section sections[ARRAY_SIZE(razor_sections)];
	struct hashtable table;
	struct array pool, *arrays[ARRAY_SIZE(razor_sections)];
	uint32_t offset;
	int count, i, j;
	static const char padding[4];

	array_init(&pool);
	hashtable_init(&table, &pool);

	j = 0;
	for (i = 0; i < ARRAY_SIZE(razor_sections); i++) {
		if ((razor_sections[i].flags & section_mask) == 0)
			continue;

		arrays[j] = (void *) set + razor_sections[i].offset;
		sections[j].name =
			hashtable_tokenize(&table, razor_sections[i].name);
		j++;
	}

	count = j;
	header.magic = RAZOR_MAGIC;
	header.version = RAZOR_VERSION;
	header.num_sections = count;
	offset = sizeof header + count * sizeof *sections + ALIGN(pool.size, 4);

	for (i = 0; i < count; i++) {
		sections[i].offset = offset;
		sections[i].size = arrays[i]->size;
		offset += ALIGN(arrays[i]->size, 4);
	}

	razor_write(fd, &header, sizeof header);
	razor_write(fd, sections, count * sizeof *sections);
	razor_write(fd, pool.data, pool.size);
	razor_write(fd, padding, PADDING(pool.size, 4));

	for (i = 0; i < count; i++) {
		razor_write(fd, arrays[i]->data, arrays[i]->size);
		razor_write(fd, padding, PADDING(arrays[i]->size, 4));
	}

	array_release(&pool);
	hashtable_release(&table);

	return 0;
}

RAZOR_EXPORT int
razor_set_write(struct razor_set *set, const char *filename, uint32_t sections)
{
	int fd, status;

	fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0666);
	if (fd < 0)
		return -1;

	status = razor_set_write_to_fd(set, fd, sections);
	if (status) {
	    close(fd);
	    return status;
	}

	return close(fd);
}

RAZOR_EXPORT void
razor_build_evr(char *evr_buf, int size, const char *epoch,
		const char *version, const char *release)
{
	int len;

	if (!version || !*version) {
		*evr_buf = '\0';
		return;
	}

	if (epoch && *epoch && strcmp(epoch, "0") != 0) {
		len = snprintf(evr_buf, size, "%s:", epoch);
		evr_buf += len;
		size -= len;
	}
	len = snprintf(evr_buf, size, "%s", version);
	evr_buf += len;
	size -= len;
	if (release && *release)
		snprintf(evr_buf, size, "-%s", release);
}

RAZOR_EXPORT int
razor_versioncmp(const char *s1, const char *s2)
{
	const char *p1, *p2;
	long n1, n2;
	int res;

	assert (s1 != NULL);
	assert (s2 != NULL);

	n1 = strtol(s1, (char **) &p1, 10);
	n2 = strtol(s2, (char **) &p2, 10);

	/* Epoch; if one but not the other has an epoch set, default
	 * the epoch-less version to 0. */
	res = (*p1 == ':') - (*p2 == ':');
	if (res < 0) {
		n1 = 0;
		p1 = s1;
		p2++;
	} else if (res > 0) {
		p1++;
		n2 = 0;
		p2 = s2;
	}

	if (n1 != n2)
		return n1 - n2;
	while (*p1 && *p2) {
		if (*p1 != *p2)
			return *p1 - *p2;
		p1++;
		p2++;
		if (isdigit(*p1) && isdigit(*p2))
			return razor_versioncmp(p1, p2);
	}

	return *p1 - *p2;
}

static const char *
razor_package_get_details_type(struct razor_set *set,
			       struct razor_package *package,
			       enum razor_detail_type type)
{
	const char *pool;

	switch (type) {
	case RAZOR_DETAIL_NAME:
		pool = set->string_pool.data;
		return &pool[package->name];

	case RAZOR_DETAIL_VERSION:
		pool = set->string_pool.data;
		return &pool[package->version];

	case RAZOR_DETAIL_ARCH:
		pool = set->string_pool.data;
		return &pool[package->arch];

	case RAZOR_DETAIL_SUMMARY:
		pool = set->details_string_pool.data;
		return &pool[package->summary];

	case RAZOR_DETAIL_DESCRIPTION:
		pool = set->details_string_pool.data;
		return &pool[package->description];

	case RAZOR_DETAIL_URL:
		pool = set->details_string_pool.data;
		return &pool[package->url];

	case RAZOR_DETAIL_LICENSE:
		pool = set->details_string_pool.data;
		return &pool[package->license];

	default:
		fprintf(stderr, "type %u not found\n", type);
		return NULL;
	}
}

/**
 * razor_package_get_details_varg:
 * @set: a %razor_set
 * @package: a %razor_package
 * @args: a va_list of arguments to set
 **/
void
razor_package_get_details_varg(struct razor_set *set,
			       struct razor_package *package,
			       va_list args)
{
	int i;
	enum razor_detail_type type;
	const char **data;

	for (i = 0;; i += 2) {
		type = va_arg(args, enum razor_detail_type);
		if (type == RAZOR_DETAIL_LAST)
			break;
		data = va_arg(args, const char **);
		*data = razor_package_get_details_type(set, package, type);
	}

}

/**
 * razor_package_get_details:
 * @set: a %razor_set
 * @package: a %razor_package
 *
 * Gets details about a package using a varg interface
 * The vararg must be terminated with %RAZOR_DETAIL_LAST.
 *
 * Example: razor_package_get_details (set, package,
 *				       RAZOR_DETAIL_URL, &url,
 *				       RAZOR_DETAIL_LAST);
 **/
RAZOR_EXPORT void
razor_package_get_details(struct razor_set *set, struct razor_package *package, ...)
{
	va_list args;

	assert (set != NULL);
	assert (package != NULL);

	va_start(args, NULL);
	razor_package_get_details_varg (set, package, args);
	va_end (args);
}

RAZOR_EXPORT const char *
razor_property_relation_to_string(struct razor_property *p)
{
	assert (p != NULL);

	switch (p->flags & RAZOR_PROPERTY_RELATION_MASK) {
	case RAZOR_PROPERTY_LESS:
		return "<";

	case RAZOR_PROPERTY_LESS | RAZOR_PROPERTY_EQUAL:
		return "<=";

	case RAZOR_PROPERTY_EQUAL:
		return "=";

	case RAZOR_PROPERTY_GREATER | RAZOR_PROPERTY_EQUAL:
		return ">=";

	case RAZOR_PROPERTY_GREATER:
		return ">";

	default:
		return "?";
	}
}

RAZOR_EXPORT const char *
razor_property_type_to_string(struct razor_property *p)
{
	assert (p != NULL);

	switch (p->flags & RAZOR_PROPERTY_TYPE_MASK) {
	case RAZOR_PROPERTY_REQUIRES:
		return "requires";
	case RAZOR_PROPERTY_PROVIDES:
		return "provides";
	case RAZOR_PROPERTY_CONFLICTS:
		return "conflicts";
	case RAZOR_PROPERTY_OBSOLETES:
		return "obsoletes";
	default:
		return NULL;
	}
}

RAZOR_EXPORT struct razor_entry *
razor_set_find_entry(struct razor_set *set,
		     struct razor_entry *dir, const char *pattern)
{
	struct razor_entry *e;
	const char *n, *pool = set->file_string_pool.data;
	int len;

	assert (set != NULL);
	assert (dir != NULL);
	assert (pattern != NULL);

	e = (struct razor_entry *) set->files.data + dir->start;
	do {
		n = pool + e->name;
		if (strcmp(pattern + 1, n) == 0)
			return e;
		len = strlen(n);
		if (e->start != 0 && strncmp(pattern + 1, n, len) == 0 &&
		    pattern[len + 1] == '/') {
			return razor_set_find_entry(set, e, pattern + len + 1);
		}
	} while (!((e++)->flags & RAZOR_ENTRY_LAST));

	return NULL;
}

static void
list_dir(struct razor_set *set, struct razor_entry *dir,
	 char *prefix, const char *pattern)
{
	struct razor_entry *e;
	const char *n, *pool = set->file_string_pool.data;

	e = (struct razor_entry *) set->files.data + dir->start;
	do {
		n = pool + e->name;
		if (pattern && pattern[0] && fnmatch(pattern, n, 0) != 0)
			continue;
		printf("%s/%s\n", prefix, n);
		if (e->start) {
			char *sub = prefix + strlen (prefix);
			*sub = '/';
			strcpy (sub + 1, n);
			list_dir(set, e, prefix, pattern);
			*sub = '\0';
		}
	} while (!((e++)->flags & RAZOR_ENTRY_LAST));
}

RAZOR_EXPORT void
razor_set_list_files(struct razor_set *set, const char *pattern)
{
	struct razor_entry *e;
	char buffer[512], *p, *base;

	assert (set != NULL);

	if (pattern == NULL || !strcmp (pattern, "/")) {
		buffer[0] = '\0';
		list_dir(set, set->files.data, buffer, NULL);
		return;
	}

	strcpy(buffer, pattern);
	e = razor_set_find_entry(set, set->files.data, buffer);
	if (e && e->start > 0) {
		base = NULL;
	} else {
		p = strrchr(buffer, '/');
		if (p) {
			*p = '\0';
			base = p + 1;
		} else {
			base = NULL;
		}
	}
	e = razor_set_find_entry(set, set->files.data, buffer);
	if (e && e->start != 0)
		list_dir(set, e, buffer, base);
}

static struct list *
list_package_files(struct razor_set *set, struct list *r,
		   struct razor_entry *dir, uint32_t end,
		   char *prefix)
{
	struct razor_entry *e, *f, *entries;
	uint32_t next, file;
	char *pool;
	int len;

	entries = (struct razor_entry *) set->files.data;
	pool = set->file_string_pool.data;

	e = entries + dir->start;
	do {
		if (entries + r->data == e) {
			printf("%s/%s\n", prefix, pool + e->name);
			r = list_next(r);
			if (!r)
				return NULL;
			if (r->data >= end)
				return r;
		}
	} while (!((e++)->flags & RAZOR_ENTRY_LAST));

	e = entries + dir->start;
	do {
		if (e->start == 0)
			continue;

		if (e->flags & RAZOR_ENTRY_LAST)
			next = end;
		else {
			f = e + 1;
			while (f->start == 0 && !(f->flags & RAZOR_ENTRY_LAST))
				f++;
			if (f->start == 0)
				next = end;
			else
				next = f->start;
		}

		file = r->data;
		if (e->start <= file && file < next) {
			len = strlen(prefix);
			prefix[len] = '/';
			strcpy(prefix + len + 1, pool + e->name);
			r = list_package_files(set, r, e, next, prefix);
			prefix[len] = '\0';
		}
	} while (!((e++)->flags & RAZOR_ENTRY_LAST) && r != NULL);

	return r;
}

RAZOR_EXPORT void
razor_set_list_package_files(struct razor_set *set,
			     struct razor_package *package)
{
	struct list *r;
	uint32_t end;
	char buffer[512];

	assert (set != NULL);
	assert (package != NULL);

	r = list_first(&package->files, &set->file_pool);
	end = set->files.size / sizeof (struct razor_entry);
	buffer[0] = '\0';
	list_package_files(set, r, set->files.data, end, buffer);
}

/* The diff order matters.  We should sort the packages so that a
 * REMOVE of a package comes before the INSTALL, and so that all
 * requires for a package have been installed before the package.
 **/

RAZOR_EXPORT void
razor_set_diff(struct razor_set *set, struct razor_set *upstream,
	       razor_diff_callback_t callback, void *data)
{
 	struct razor_package_iterator *pi1, *pi2;
 	struct razor_package *p1, *p2;
	const char *name1, *name2, *version1, *version2, *arch1, *arch2;
	int res;

	assert (set != NULL);
	assert (upstream != NULL);

	pi1 = razor_package_iterator_create(set);
	pi2 = razor_package_iterator_create(upstream);

	razor_package_iterator_next(pi1, &p1,
				    RAZOR_DETAIL_NAME, &name1,
				    RAZOR_DETAIL_VERSION, &version1,
				    RAZOR_DETAIL_ARCH, &arch1,
				    RAZOR_DETAIL_LAST);
	razor_package_iterator_next(pi2, &p2,
				    RAZOR_DETAIL_NAME, &name2,
				    RAZOR_DETAIL_VERSION, &version2,
				    RAZOR_DETAIL_ARCH, &arch2,
				    RAZOR_DETAIL_LAST);

	while (p1 || p2) {
		if (p1 && p2) {
			res = strcmp(name1, name2);
			if (res == 0)
				res = razor_versioncmp(version1, version2);
		} else {
			res = 0;
		}

		if (p2 == NULL || res < 0)
			callback(RAZOR_DIFF_ACTION_REMOVE,
				 p1, name1, version1, arch1, data);
		else if (p1 == NULL || res > 0)
			callback(RAZOR_DIFF_ACTION_ADD,
				 p2, name2, version2, arch2, data);

		if (p1 != NULL && res <= 0)
			razor_package_iterator_next(pi1, &p1,
						    RAZOR_DETAIL_NAME, &name1,
						    RAZOR_DETAIL_VERSION, &version1,
						    RAZOR_DETAIL_ARCH, &arch1,
						    RAZOR_DETAIL_LAST);
		if (p2 != NULL && res >= 0)
			razor_package_iterator_next(pi2, &p2,
						    RAZOR_DETAIL_NAME, &name2,
						    RAZOR_DETAIL_VERSION, &version2,
						    RAZOR_DETAIL_ARCH, &arch2,
						    RAZOR_DETAIL_LAST);
	}

	razor_package_iterator_destroy(pi1);
	razor_package_iterator_destroy(pi2);
}

struct install_action {
	enum razor_install_action action;
	struct razor_package *package;
};

struct razor_install_iterator {
	struct razor_set *set;
	struct razor_set *next;
	struct array actions;
	struct install_action *a, *end;
};

static void
add_action(enum razor_diff_action action,
	   struct razor_package *package,
	   const char *name,
	   const char *version,
	   const char *arch,
	   void *data)
{
	struct razor_install_iterator *ii = data;
	struct install_action *a;

	a = array_add(&ii->actions, sizeof *a);
	a->package = package;

	switch (action) {
	case RAZOR_DIFF_ACTION_ADD:
		a->action = RAZOR_INSTALL_ACTION_ADD;
		break;
	case RAZOR_DIFF_ACTION_REMOVE:
		a->action = RAZOR_INSTALL_ACTION_REMOVE;
		break;
	}
}

RAZOR_EXPORT struct razor_install_iterator *
razor_set_create_install_iterator(struct razor_set *set,
				  struct razor_set *next)
{
	struct razor_install_iterator *ii;

	assert (set != NULL);
	assert (next != NULL);

	ii = zalloc(sizeof *ii);
	ii->set = set;
	ii->next = next;
	
	razor_set_diff(set, next, add_action, ii);

	ii->a = ii->actions.data;
	ii->end = ii->actions.data + ii->actions.size;

	/* FIXME: We need to figure out the right install order here,
	 * so the post and pre scripts can run. */

	return ii;
}

RAZOR_EXPORT int
razor_install_iterator_next(struct razor_install_iterator *ii,
			    struct razor_set **set,
			    struct razor_package **package,
			    enum razor_install_action *action,
			    int *count)
{
	if (ii->a == ii->end)
		return 0;

	switch (ii->a->action) {
	case RAZOR_INSTALL_ACTION_ADD:
		*set = ii->next;
		break;
	case RAZOR_INSTALL_ACTION_REMOVE:
		*set = ii->set;
		break;
	}

	*package = ii->a->package;
	*action = ii->a->action;
	*count = 0;
	ii->a++;

	return 1;
}

RAZOR_EXPORT void
razor_install_iterator_destroy(struct razor_install_iterator *ii)
{
	array_release(&ii->actions);
	free(ii);
}
