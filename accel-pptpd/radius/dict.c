#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#include "list.h"
#include "radius_p.h"
#include "log.h"

#include "memdebug.h"

static struct rad_dict_t *dict;

static char *skip_word(char *ptr)
{
	for(; *ptr; ptr++)
		if (*ptr == ' ' || *ptr == '\t' || *ptr == '\n') 
			break;
	return ptr;
}
static char *skip_space(char *ptr)
{
	for(; *ptr; ptr++)
		if (*ptr != ' ' && *ptr != '\t')
			break;
	return ptr;
}
static int split(char *buf, char **ptr)
{
	int i;

	for (i = 0; i < 3; i++) {
		buf = skip_word(buf);
		if (!*buf)
			return i;
		
		*buf = 0;
		
		buf = skip_space(buf + 1);
		if (!*buf)
			return i;

		ptr[i] = buf;
	}

	buf = skip_word(buf);
	//if (*buf == '\n')
		*buf = 0;
	//else if (*buf)
	//	return -1;

	return i;
}

struct rad_dict_attr_t *find_attr(struct list_head *items, const char *name)
{
	struct rad_dict_attr_t *attr;

	list_for_each_entry(attr, items, entry)
		if (!strcmp(attr->name, name))
			return attr;

	return NULL;
}

#define BUF_SIZE 1024

static char *path, *fname1, *buf;
static int dict_load(const char *fname)
{
	FILE *f;
	char *ptr[3], *endptr;
	int r, n = 0;
	struct rad_dict_attr_t *attr;
	struct rad_dict_value_t *val;
	struct rad_dict_vendor_t *vendor;
	struct list_head *items;

	f = fopen(fname, "r");
	if (!f) {
		log_error("radius: open dictioanary '%s': %s\n", fname, strerror(errno));
		return -1;
	}
	
	items = &dict->items;

	while (fgets(buf, BUF_SIZE, f)) {
		n++;
		if (buf[0] == '#' || buf[0] == '\n' || buf[0] == 0)
			continue;
		r = split(buf, ptr);
		if (r == 1) {
			if (!strcmp(buf, "BEGIN-VENDOR")) {
				vendor = rad_dict_find_vendor_name(ptr[0]);
				if (!vendor) {
					log_error("radius:%s:%i: vendor not found\n", fname, n);
					goto out_err;
				}
				items = &vendor->items;
			} else if (!strcmp(buf, "END-VENDOR"))
				items = &dict->items;
			else if (!strcmp(buf, "$INCLUDE")) {
				for (r = strlen(path) - 1; r; r--)
					if (path[r] == '/') {
						path[r + 1] = 0;
						break;
					}
				strcpy(fname1, path);
				strcat(fname1, ptr[0]);
				if (dict_load(fname1))
					goto out_err;
			} else
				goto out_err_syntax;
		} else if (r == 2) {
			if (!strcmp(buf, "VENDOR")) {
				vendor = malloc(sizeof(*vendor));
				if (!vendor) {
					log_emerg("radius: out of memory\n");
					goto out_err;
				}
				vendor->id = strtol(ptr[1], &endptr, 10);
				if (*endptr != 0)
					goto out_err_syntax;
				vendor->name = strdup(ptr[0]);
				if (!vendor->name) {
					log_emerg("radius: out of memory\n");
					goto out_err;
				}
				INIT_LIST_HEAD(&vendor->items);
				list_add_tail(&vendor->entry, &dict->vendors);
			} else
				goto out_err_syntax;
		} else if (r == 3) {
			if (!strcmp(buf, "ATTRIBUTE")) {
				attr = malloc(sizeof(*attr));
				if (!attr) {
					log_emerg("radius: out of memory\n");
					goto out_err;
				}
				memset(attr, 0, sizeof(*attr));
				INIT_LIST_HEAD(&attr->values);
				list_add_tail(&attr->entry, items);
				attr->name = strdup(ptr[0]);
				attr->id = strtol(ptr[1], &endptr, 10);
				if (*endptr != 0)
					goto out_err_syntax;
				if (!strcmp(ptr[2], "integer"))
					attr->type = ATTR_TYPE_INTEGER;
				else if (!strcmp(ptr[2], "string"))
					attr->type = ATTR_TYPE_STRING;
				else if (!strcmp(ptr[2], "date"))
					attr->type = ATTR_TYPE_DATE;
				else if (!strcmp(ptr[2], "ipaddr"))
					attr->type = ATTR_TYPE_IPADDR;
				else if (!strcmp(ptr[2], "octets"))
					attr->type = ATTR_TYPE_OCTETS;
				else {
					log_error("radius:%s:%i: unknown attribute type\n", fname, n);
					goto out_err;
				}
			} else if (!strcmp(buf, "VALUE")) {
				attr = find_attr(items, ptr[0]);
				if (!attr) {
					log_error("radius:%s:%i: unknown attribute\n", fname, n);
					goto out_err;
				}
				val = malloc(sizeof(*val));
				if (!val) {
					log_emerg("radius: out of memory\n");
					goto out_err;
				}
				memset(val, 0, sizeof(*val));
				list_add_tail(&val->entry, &attr->values);
				val->name = strdup(ptr[1]);
				switch (attr->type) {
					case ATTR_TYPE_INTEGER:
						val->val.integer = strtol(ptr[2], &endptr, 10);
						if (*endptr != 0)
							goto out_err_syntax;
						break;
					case ATTR_TYPE_STRING:
						val->val.string = strdup(ptr[2]);
						break;
					case ATTR_TYPE_DATE:
						log_warn("radius:%s:%i: VALUE of type 'date' is not implemented yet\n", fname, n);
						break;
					case ATTR_TYPE_IPADDR:
						log_warn("radius:%s:%i: VALUE of type 'ipaddr' is not implemented yet\n", fname, n);
						break;
				}
			} else
				goto out_err_syntax;
		} else
			goto out_err_syntax;
	}

	fclose(f);

	return 0;

out_err_syntax:
	log_error("radius:%s:%i: syntaxis error\n", fname, n);
out_err:
	fclose(f);
	return -1;
}

int rad_dict_load(const char *fname)
{
	int r = -1;

	dict = malloc(sizeof(*dict));
	if (!dict) {
		log_emerg("radius: out of memory\n");
		return -1;
	}
	INIT_LIST_HEAD(&dict->items);
	INIT_LIST_HEAD(&dict->vendors);

	path = _malloc(PATH_MAX);
	if (!path) {
		log_emerg("radius: out of memory\n");
		goto out_free_dict;
	}

	fname1 = _malloc(PATH_MAX);
	if (!fname1) {
		log_emerg("radius: out of memory\n");
		goto out_free_path;
	}

	buf = _malloc(BUF_SIZE);
	if (!buf) {
		log_emerg("radius: out of memory\n");
		goto out_free_fname1;
	}

	strcpy(path, fname);

	r = dict_load(fname);

out_free_fname1:
	_free(fname1);
out_free_path:
	_free(path);
out_free_dict:
	if (r)
		rad_dict_free(dict);
	return r;
}

void rad_dict_free(struct rad_dict_t *dict)
{
	struct rad_dict_attr_t *attr;
	struct rad_dict_value_t *val;

	while (!list_empty(&dict->items)) {
		attr = list_entry(dict->items.next, typeof(*attr), entry);
		while (!list_empty(&attr->values)) {
			val = list_entry(attr->values.next, typeof(*val), entry);
			list_del(&val->entry);
			_free((char*)val->name);
			if (attr->type == ATTR_TYPE_STRING)
				_free((char*)val->val.string);
			_free(val);
		}
		list_del(&attr->entry);
		_free((char*)attr->name);
		_free(attr);
	}
	_free(dict);
}

static struct rad_dict_attr_t *dict_find_attr(struct list_head *items, const char *name)
{
	struct rad_dict_attr_t *attr;

	list_for_each_entry(attr, items, entry)
		if (!strcmp(attr->name, name))
			return attr;

	return NULL;
}

__export struct rad_dict_attr_t *rad_dict_find_attr(const char *name)
{
	return dict_find_attr(&dict->items, name);
}

__export struct rad_dict_attr_t *rad_dict_find_attr_id(struct rad_dict_vendor_t *vendor, int id)
{
	struct rad_dict_attr_t *attr;
	struct list_head *items = vendor ? &vendor->items : &dict->items;
	
	list_for_each_entry(attr, items, entry)
		if (attr->id == id)
			return attr;

	return NULL;
}

__export struct rad_dict_value_t *rad_dict_find_val_name(struct rad_dict_attr_t *attr, const char *name)
{
	struct rad_dict_value_t *val;

	list_for_each_entry(val, &attr->values, entry)
		if (!strcmp(val->name, name))
			return val;

	return NULL;
}

__export struct rad_dict_value_t *rad_dict_find_val(struct rad_dict_attr_t *attr, rad_value_t v)
{
	struct rad_dict_value_t *val;

	if (attr->type != ATTR_TYPE_INTEGER)
		return NULL;

	list_for_each_entry(val, &attr->values, entry)
		if (val->val.integer == v.integer)
			return val;

	return NULL;
}

__export struct rad_dict_vendor_t *rad_dict_find_vendor_name(const char *name)
{
	struct rad_dict_vendor_t *vendor;

	list_for_each_entry(vendor, &dict->vendors, entry) {
		if (!strcmp(vendor->name, name))
			return vendor;
	}

	return NULL;
}

__export struct rad_dict_vendor_t *rad_dict_find_vendor_id(int id)
{
	struct rad_dict_vendor_t *vendor;

	list_for_each_entry(vendor, &dict->vendors, entry) {
		if (vendor->id == id)
			return vendor;
	}

	return NULL;
}

__export struct rad_dict_attr_t *rad_dict_find_vendor_attr(struct rad_dict_vendor_t *vendor, const char *name)
{
	return dict_find_attr(&vendor->items, name);
}

