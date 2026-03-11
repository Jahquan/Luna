/****************************************************************************
 * The contents of this file are subject to the Mozilla Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code is Luna code.
 *
 * The Initial Developer of the Original Code is Olivier ARMAND
 * <olivier.calc@gmail.com>.
 * Portions created by the Initial Developer are Copyright (C) 2011-2014
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include "DES.h"
#include <zlib.h>
#include "minizip-1.1/zip.h"
#include "third_party/expat/lib/expat.h"

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <sys/stat.h>
#define PATH_SEP '\\'
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define PATH_SEP '/'
#endif

#define LUNA_VER "2.1"

#ifndef min
 #define min(a,b) (((a) < (b)) ? (a) : (b))
#endif

const char *gnu_basename(const char *path)
{
	char *base = strrchr(path, '/');
	char *backslash_base = strrchr(path, '\\');
	if (!base || (backslash_base && backslash_base > base))
		base = backslash_base;
	return base ? base+1 : path;
}

int unlink_path(const char *path);
int replace_file_path(const char *source_path, const char *target_path);

void print_usage(void) {
	puts("Luna v" LUNA_VER " usage:\n"
				 "  luna [INFILE.lua|INFILE.py|Problem1.xml|Document.xml|ABCD.BMP]\n"
				 "  luna [INFILE.lua|-] [OUTFILE.tns]\n"
				 "  luna [INFILE.py]* [OUTFILE.tns]\n"
				 "  luna DIRECTORY [...DIRECTORY]\n"
				 "  luna [Problem1.xml|Document.xml|ABCD.BMP]* [OUTFILE.tns]\n"
				 "Converts a Lua script, Python scripts, or XML problems/documents/resources to a TNS document.\n"
				 "If no OUTFILE.tns is given for a single input file, Luna writes a .tns next to that input.\n"
				 "If any input is a directory, Luna recursively converts every .py file it finds in place.\n"
				 "If the input file '-', reads it as Lua from the standard input.\n"
				 "For Python, the first script will be the one that shows when the TNS document is opened.\n"
				 "A default Document.xml will be generated if not specified.");
}

/* Reads an UTF-8 character from in to *c. Doesn't read at or after end. Returns a pointer to the next character. */
char *utf82unicode(char *in, char *end, unsigned long *c) {
	if (in == end) {
		*c = 0;
		return in;
	}
	if (!(*in & 0b10000000)) {
		*c = *in;
		return in + 1;
	}
	if ((*in & 0b11100000) == 0b11000000) {
		*c = (*in & 0b00011111) << 6;
		if (end > in + 1)
			*c |= *(in + 1) & 0b00111111;
		return min(end, in + 2);
	}
	if ((*in & 0b11110000) == 0b11100000) {
		*c = (*in & 0b00001111) << 12;
		if (end > in + 1)
			*c |= (*(in + 1) & 0b00111111) << 6;
		if (end > in + 2)
			*c |= *(in + 2) & 0b00111111;
		return min(end, in + 3);
	}
	if ((*in & 0b11111000) == 0b11110000) {
		*c = (*in & 0b00000111) << 18;
		if (end > in + 1)
			*c |= (*(in + 1) & 0b00111111) << 12;
		if (end > in + 2)
			*c |= (*(in + 2) & 0b00111111) << 6;
		if (end > in + 3)
			*c |= *(in + 3) & 0b00111111;
		return min(end, in + 4);
	}
	*c = 0;
	return in + 1;
}

/* sub-routine of read_file_and_xml_compress() to escape the unicode characters as required by the XML compression. Returns the new in_buf or NULL */
void *escape_unicode(char *in_buf, size_t header_size, size_t footer_size, size_t in_size, size_t *obuf_size) {
	char *p, *op;
	char *out_buf = malloc(header_size + in_size * 4 /* worst case */ + footer_size);
	if (!out_buf) {
		puts("escape_unicode: can't malloc");
		return NULL;
	}
	memcpy(out_buf, in_buf, header_size);

	p = in_buf + header_size;
	if (in_size >= 3 && !memcmp(in_buf + header_size, "\xEF\xBB\xBF", 3)) // skip the UTF-8 BOM if any
		p += 3;
	for (op = out_buf + header_size; p < in_buf + header_size + in_size;) {
		unsigned long uc;
		p = utf82unicode(p, in_buf + header_size + in_size, &uc);
		if (uc < 0x80) {
			*op++ = (char)uc;
		} else if (uc < 0x800) {
			*op++ = (char)(uc >> 8);
			*op++ = (char)(uc);
		} else if (uc < 0x10000) {
			*op++ = 0b10000000;
			*op++ = (char)(uc >> 8);
			*op++ = (char)(uc);
		} else {
			*op++ = 0b00001000;
			*op++ = (char)(uc >> 16);
			*op++ = (char)(uc >> 8);
			*op++ = (char)(uc);
		}
	}
	*obuf_size = op - out_buf + footer_size;
	char *out_buf2 = realloc(out_buf, *obuf_size);
	if (!out_buf2) {
		free(out_buf);
		puts("escape_unicode: can't realloc");
		return NULL;
	}
	return out_buf2;
}

/* sub-routine of xml_compress() to fix occurrences of CDATA end sequence "]]>" in Lua scripts
 * by spitting them between two CDATA sections. Returns the new in_buf, or NULL if out of memory. */
static const char cdata_restart[] = "]]><![CDATA[";
void *fix_cdata_end_seq(char *in_buf, size_t header_size, size_t in_size, size_t *obuf_size) {
	for (size_t offset = header_size; offset < header_size + in_size - 2; offset++) {
		if (!memcmp(in_buf + offset, "]]>", 3)) {
			// Skip "]]"
			offset += 2;
			// Insert "]]><![CDATA["
			*obuf_size += sizeof(cdata_restart) - 1;
			char *new_in_buf;
			if (!(new_in_buf = realloc(in_buf, *obuf_size))) {
				puts("can't realloc in_buf");
				free(in_buf);
				return NULL;
			}
			in_buf = new_in_buf;
			memmove(in_buf + offset + sizeof(cdata_restart) - 1, in_buf + offset, header_size + in_size - offset);
			memcpy(in_buf + offset, cdata_restart, sizeof(cdata_restart) - 1);
			in_size += sizeof(cdata_restart) - 1;
			offset += sizeof(cdata_restart) - 1;
		}
	}
	return in_buf;
}

typedef struct {
	XML_Parser parser;
	const char *inf_path;
	const char *source_start;
	size_t source_size;
	char *out_ptr;
	size_t out_capacity;
	size_t size_written;
	unsigned tagid_stack[100];
	unsigned tagid_head_index;
	unsigned last_tagid;
	int copy_started;
	const char *error_reason;
} xml_reformat_ctx;

void print_invalid_xml_doc(const char *inf_path, unsigned long line, unsigned long column, const char *reason) {
	if (inf_path)
		printf("input file '%s' is not a valid XML document at line %lu column %lu", inf_path, line, column);
	else
		printf("input problem/document is not a valid XML document at line %lu column %lu", line, column);

	if (reason && *reason)
		printf(": %s\n", reason);
	else
		putchar('\n');
}

void stop_xml_parse(xml_reformat_ctx *ctx, const char *reason) {
	ctx->error_reason = reason;
	XML_StopParser(ctx->parser, XML_FALSE);
}

int append_xml_bytes(xml_reformat_ctx *ctx, const char *data, size_t size) {
	if (ctx->size_written + size > ctx->out_capacity) {
		stop_xml_parse(ctx, "XML output exceeds converter buffer");
		return 0;
	}
	memcpy(ctx->out_ptr + ctx->size_written, data, size);
	ctx->size_written += size;
	return 1;
}

int current_xml_event_is_self_closing(xml_reformat_ctx *ctx) {
	XML_Index byte_index = XML_GetCurrentByteIndex(ctx->parser);
	int byte_count = XML_GetCurrentByteCount(ctx->parser);
	const char *ptr;

	if (byte_index < 0 || byte_count <= 0)
		return 0;

	ptr = ctx->source_start + byte_index + byte_count - 1;
	while (ptr > ctx->source_start + byte_index && isspace((unsigned char)ptr[-1]))
		ptr--;
	return ptr > ctx->source_start + byte_index && ptr[-1] == '/';
}

void XMLCALL xml_default_handler(void *userData, const XML_Char *s, int len) {
	xml_reformat_ctx *ctx = userData;
	if (!ctx->copy_started || len <= 0)
		return;
	append_xml_bytes(ctx, s, len);
}

void XMLCALL xml_start_element(void *userData, const XML_Char *name, const XML_Char **atts) {
	xml_reformat_ctx *ctx = userData;
	(void)atts;

	if (!ctx->copy_started) {
		if (strcmp(name, "prob") && strcmp(name, "doc")) {
			stop_xml_parse(ctx, "input is not a TI-Nspire problem/document");
			return;
		}
		ctx->copy_started = 1;
	}

	XML_DefaultCurrent(ctx->parser);
	if (current_xml_event_is_self_closing(ctx))
		return;

	if (ctx->tagid_head_index >= sizeof(ctx->tagid_stack) / sizeof(*ctx->tagid_stack)) {
		stop_xml_parse(ctx, "XML nesting exceeds converter limit");
		return;
	}

	ctx->tagid_stack[ctx->tagid_head_index++] = ctx->last_tagid++;
}

void XMLCALL xml_end_element(void *userData, const XML_Char *name) {
	xml_reformat_ctx *ctx = userData;
	(void)name;

	if (XML_GetCurrentByteCount(ctx->parser) == 0)
		return;

	if (ctx->tagid_head_index == 0) {
		stop_xml_parse(ctx, "unexpected closing tag");
		return;
	}

	unsigned index = ctx->tagid_stack[--ctx->tagid_head_index];
	if (index < 256) {
		char end_tag[2];
		end_tag[0] = 0x0E;
		end_tag[1] = index;
		append_xml_bytes(ctx, end_tag, sizeof(end_tag));
	}
	else
		XML_DefaultCurrent(ctx->parser);
}

void XMLCALL xml_start_doctype(void *userData, const XML_Char *doctypeName, const XML_Char *sysid, const XML_Char *pubid, int has_internal_subset) {
	xml_reformat_ctx *ctx = userData;
	(void)doctypeName;
	(void)sysid;
	(void)pubid;
	(void)has_internal_subset;
	stop_xml_parse(ctx, "DOCTYPE is not supported");
}

/* sub-routine of read_file_and_xml_compress() in case of an XML problem as input. Returns the new in_buf or NULL */
void *reformat_xml_doc(char *in_buf, size_t header_size, size_t in_size, size_t *obuf_size, const char *inf_path) {
	char *out_buf = malloc(header_size + in_size);
	xml_reformat_ctx ctx;
	enum XML_Status parse_status;

	if (!out_buf) {
		puts("reformat_xml_doc: can't alloc");
		free(in_buf);
		return NULL;
	}

	memcpy(out_buf, in_buf, header_size);
	memset(&ctx, 0, sizeof(ctx));
	ctx.inf_path = inf_path;
	ctx.source_start = in_buf + header_size;
	ctx.source_size = in_size;
	ctx.out_ptr = out_buf + header_size;
	ctx.out_capacity = in_size;

	ctx.parser = XML_ParserCreate(NULL);
	if (!ctx.parser) {
		puts("reformat_xml_doc: can't create XML parser");
		free(out_buf);
		free(in_buf);
		return NULL;
	}

	XML_SetUserData(ctx.parser, &ctx);
	XML_SetDefaultHandler(ctx.parser, xml_default_handler);
	XML_SetElementHandler(ctx.parser, xml_start_element, xml_end_element);
	XML_SetStartDoctypeDeclHandler(ctx.parser, xml_start_doctype);

	parse_status = XML_Parse(ctx.parser, ctx.source_start, ctx.source_size, XML_TRUE);
	if (parse_status != XML_STATUS_OK || ctx.error_reason) {
		const char *reason = ctx.error_reason;
		if (!reason) {
			enum XML_Error error = XML_GetErrorCode(ctx.parser);
			reason = XML_ErrorString(error);
		}
		if (reason && !strcmp(reason, "input is not a TI-Nspire problem/document")) {
			if (inf_path)
				printf("input file '%s' is not a TI-Nspire problem/document\n", inf_path);
			else
				puts("input is not a TI-Nspire problem/document");
		}
		else {
			unsigned long line = XML_GetCurrentLineNumber(ctx.parser);
			unsigned long column = XML_GetCurrentColumnNumber(ctx.parser) + 1;
			print_invalid_xml_doc(inf_path, line ? line : 1, column ? column : 1, reason);
		}
		XML_ParserFree(ctx.parser);
		free(out_buf);
		free(in_buf);
		return NULL;
	}

	XML_ParserFree(ctx.parser);
	*obuf_size = header_size + ctx.size_written;
	char *resized_out_buf = realloc(out_buf, *obuf_size);
	if (!resized_out_buf) {
		puts("reformat_xml_doc: can't realloc out_buf");
		free(out_buf);
		free(in_buf);
		return NULL;
	}
	free(in_buf);
	return resized_out_buf;
}

// ext must start with "."
int has_ext(const char *filepath, const char *ext) {
	return strlen(filepath) > strlen(ext) && !strcasecmp(ext, filepath + strlen(filepath) - strlen(ext));
}

char *copy_filepath(const char *filepath) {
	size_t filepath_size = strlen(filepath) + 1;
	char *copied_path = malloc(filepath_size);
	if (!copied_path) {
		puts("can't malloc copied_path");
		return NULL;
	}
	memcpy(copied_path, filepath, filepath_size);
	return copied_path;
}

char *create_temp_outfile_path(const char *outfile_path) {
#ifdef _WIN32
	size_t temp_path_size = strlen(outfile_path) + 32;
	char *temp_path = malloc(temp_path_size);
	if (!temp_path) {
		puts("can't malloc temp_path");
		return NULL;
	}
	for (unsigned attempt = 0; attempt < 1000; attempt++) {
		snprintf(temp_path, temp_path_size, "%s.tmp.%08lx%03u", outfile_path,
			(unsigned long)GetCurrentProcessId(), attempt);
		HANDLE temp_file = CreateFileA(temp_path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
			CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, NULL);
		if (temp_file != INVALID_HANDLE_VALUE) {
			CloseHandle(temp_file);
			return temp_path;
		}
		if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
			break;
	}
	puts("can't create temporary output file");
	free(temp_path);
	return NULL;
#else
	static const char temp_suffix[] = ".tmp.XXXXXX";
	size_t temp_path_size = strlen(outfile_path) + sizeof(temp_suffix);
	char *temp_path = malloc(temp_path_size);
	if (!temp_path) {
		puts("can't malloc temp_path");
		return NULL;
	}
	snprintf(temp_path, temp_path_size, "%s%s", outfile_path, temp_suffix);
	int temp_fd = mkstemp(temp_path);
	if (temp_fd < 0) {
		puts("can't create temporary output file");
		free(temp_path);
		return NULL;
	}
	close(temp_fd);
	unlink_path(temp_path);
	return temp_path;
#endif
}

int path_is_qualified(const char *path) {
	return strchr(path, '/') || strchr(path, '\\') ||
		(strlen(path) >= 2 && isalpha((unsigned char)path[0]) && path[1] == ':');
}

char *resolve_single_infile_outfile_path(const char *infile_path, const char *outfile_name) {
	if (!outfile_name && !strcmp(infile_path, "-")) {
		puts("reading from standard input requires an explicit output path");
		return NULL;
	}

	if (outfile_name && (!strcmp(infile_path, "-") || path_is_qualified(outfile_name)))
		return copy_filepath(outfile_name);

	const char *basename = gnu_basename(infile_path);
	size_t dir_size = basename - infile_path;
	size_t stem_size;
	if (outfile_name) {
		stem_size = strlen(outfile_name);
	}
	else {
		const char *ext = strrchr(basename, '.');
		stem_size = ext ? (size_t)(ext - basename) : strlen(basename);
	}
	char *outfile_path = malloc(dir_size + stem_size + (outfile_name ? 1 : sizeof(".tns")));
	if (!outfile_path) {
		puts("can't malloc outfile_path");
		return NULL;
	}
	memcpy(outfile_path, infile_path, dir_size);
	memcpy(outfile_path + dir_size, outfile_name ? outfile_name : basename, stem_size);
	if (outfile_name)
		outfile_path[dir_size + stem_size] = '\0';
	else
		memcpy(outfile_path + dir_size + stem_size, ".tns", sizeof(".tns"));
	return outfile_path;
}

char *join_filepath(const char *dirpath, const char *name) {
	size_t dirpath_size = strlen(dirpath);
	size_t name_size = strlen(name) + 1;
	int needs_sep = dirpath_size > 0 && dirpath[dirpath_size - 1] != '/' && dirpath[dirpath_size - 1] != '\\';
	char *joined_path = malloc(dirpath_size + needs_sep + name_size);
	if (!joined_path) {
		puts("can't malloc joined_path");
		return NULL;
	}
	memcpy(joined_path, dirpath, dirpath_size);
	if (needs_sep)
		joined_path[dirpath_size++] = PATH_SEP;
	memcpy(joined_path + dirpath_size, name, name_size);
	return joined_path;
}

typedef struct {
	char **names;
	size_t count;
	size_t capacity;
} directory_entry_list;

typedef struct {
	int converted;
	int failed;
	int skipped;
} recursive_batch_stats;

typedef struct {
	int is_directory;
	int is_symlink;
} path_info;

int directory_entry_cmp(const void *left, const void *right) {
	const char *const *left_name = left;
	const char *const *right_name = right;
	return strcmp(*left_name, *right_name);
}

int append_directory_entry(directory_entry_list *list, const char *name) {
	if (list->count == list->capacity) {
		size_t new_capacity = list->capacity ? list->capacity * 2 : 16;
		char **new_names = realloc(list->names, new_capacity * sizeof(*new_names));
		if (!new_names) {
			puts("can't realloc directory entry list");
			return 1;
		}
		list->names = new_names;
		list->capacity = new_capacity;
	}

	list->names[list->count] = copy_filepath(name);
	if (!list->names[list->count])
		return 1;
	list->count++;
	return 0;
}

void free_directory_entries(directory_entry_list *list) {
	for (size_t i = 0; i < list->count; i++)
		free(list->names[i]);
	free(list->names);
	list->names = NULL;
	list->count = list->capacity = 0;
}

int collect_directory_entries(const char *dirpath, directory_entry_list *list) {
#ifdef _WIN32
	char *pattern = join_filepath(dirpath, "*");
	if (!pattern)
		return 1;
	WIN32_FIND_DATAA find_data;
	HANDLE handle = FindFirstFileA(pattern, &find_data);
	free(pattern);
	if (handle == INVALID_HANDLE_VALUE) {
		DWORD error = GetLastError();
		if (error == ERROR_FILE_NOT_FOUND)
			return 0;
		printf("can't list directory '%s'\n", dirpath);
		return 1;
	}

	do {
		if (!strcmp(find_data.cFileName, ".") || !strcmp(find_data.cFileName, ".."))
			continue;
		if (append_directory_entry(list, find_data.cFileName)) {
			FindClose(handle);
			return 1;
		}
	} while (FindNextFileA(handle, &find_data));
	FindClose(handle);
#else
	DIR *dir = opendir(dirpath);
	if (!dir) {
		printf("can't open directory '%s': %s\n", dirpath, strerror(errno));
		return 1;
	}

	struct dirent *entry;
	while ((entry = readdir(dir))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		if (append_directory_entry(list, entry->d_name)) {
			closedir(dir);
			return 1;
		}
	}
	closedir(dir);
#endif

	qsort(list->names, list->count, sizeof(*list->names), directory_entry_cmp);
	return 0;
}

int get_path_info(const char *path, path_info *info) {
#ifdef _WIN32
	DWORD attrs = GetFileAttributesA(path);
	if (attrs == INVALID_FILE_ATTRIBUTES)
		return 1;
	info->is_directory = !!(attrs & FILE_ATTRIBUTE_DIRECTORY);
	info->is_symlink = !!(attrs & FILE_ATTRIBUTE_REPARSE_POINT);
#else
	struct stat st;
	if (lstat(path, &st))
		return 1;
	info->is_directory = S_ISDIR(st.st_mode);
	info->is_symlink = S_ISLNK(st.st_mode);
#endif
	return 0;
}

int path_is_directory(const char *path) {
	path_info info;
	return !get_path_info(path, &info) && info.is_directory;
}

int unlink_path(const char *path) {
#ifdef _WIN32
	return DeleteFileA(path) ? 0 : -1;
#else
	return unlink(path);
#endif
}

int replace_file_path(const char *source_path, const char *target_path) {
#ifdef _WIN32
	return MoveFileExA(source_path, target_path, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
	return rename(source_path, target_path);
#endif
}

void print_recursive_summary(const recursive_batch_stats *stats) {
	printf("recursive conversion summary: converted %d, failed %d, skipped %d\n",
		stats->converted, stats->failed, stats->skipped);
}

char *encode_python_name_text(const char *filename, size_t *encoded_size) {
	size_t filename_len = strlen(filename);
	char *escaped_filename = malloc(filename_len * 5 + 1); // "&amp;"
	if (!escaped_filename) {
		puts("can't malloc escaped_filename");
		return NULL;
	}

	char *out = escaped_filename;
	for (const char *in = filename; *in; in++) {
		const char *replacement = NULL;
		if (*in == '&')
			replacement = "&amp;";
		else if (*in == '<')
			replacement = "&lt;";
		else if (*in == '>')
			replacement = "&gt;";

		if (replacement) {
			size_t replacement_len = strlen(replacement);
			memcpy(out, replacement, replacement_len);
			out += replacement_len;
		}
		else {
			*out++ = *in;
		}
	}
	*out = '\0';

	*encoded_size = out - escaped_filename;
	char *encoded_filename = escape_unicode(escaped_filename, 0, 0, *encoded_size, encoded_size);
	free(escaped_filename);
	return encoded_filename;
}

/* Returns the output buffer, NULL on error. Fills obuf_size.
 * Don't compress anything if not Lua/XML */
void *read_file_and_xml_compress(const char *inf_path, size_t *obuf_size, const char **filename) {
	static const char lua_header[] =
		"\x54\x49\x58\x43\x30\x31\x30\x30\x2D\x31\x2E\x30\x3F\x3E\x3C\x70\x72"
		"\x6F\x62\x20\x78\x6D\x6C\x6E\x73\x3D\x22\x75\x72\x6E\x3A\x54\x49\x2E"
		"\x50\xA8\x5F\x5B\x1F\x0A\x22\x20\x76\x65\x72\x3D\x22\x31\x2E\x30\x22"
		"\x20\x70\x62\x6E\x61\x6D\x65\x3D\x22\x22\x3E\x3C\x73\x79\x6D\x3E\x0E"
		"\x01\x3C\x63\x61\x72\x64\x20\x63\x6C\x61\x79\x3D\x22\x30\x22\x20\x68"
		"\x31\x3D\x22\xF1\x00\x00\xFF\x22\x20\x68\x32\x3D\x22\xF1\x00\x00\xFF"
		"\x22\x20\x77\x31\x3D\x22\xF1\x00\x00\xFF\x22\x20\x77\x32\x3D\x22\xF1"
		"\x00\x00\xFF\x22\x3E\x3C\x69\x73\x44\x75\x6D\x6D\x79\x43\x61\x72\x64"
		"\x3E\x30\x0E\x03\x3C\x66\x6C\x61\x67\x3E\x30\x0E\x04\x3C\x77\x64\x67"
		"\x74\x20\x78\x6D\x6C\x6E\x73\x3A\x73\x63\x3D\x22\x75\x72\x6E\x3A\x54"
		"\x49\x2E\x53\xAC\x84\xF2\x2A\x41\x70\x70\x22\x20\x74\x79\x70\x65\x3D"
		"\x22\x54\x49\x2E\x53\xAC\x84\xF2\x2A\x41\x70\x70\x22\x20\x76\x65\x72"
		"\x3D\x22\x31\x2E\x30\x22\x3E\x3C\x73\x63\x3A\x6D\x46\x6C\x61\x67\x73"
		"\x3E\x30\x0E\x06\x3C\x73\x63\x3A\x76\x61\x6C\x75\x65\x3E\x2D\x31\x0E"
		"\x07\x3C\x73\x63\x3A\x73\x63\x72\x69\x70\x74\x20\x76\x65\x72\x73\x69"
		"\x6F\x6E\x3D\x22\x35\x31\x32\x22\x20\x69\x64\x3D\x22\x30\x22\x3E"
		"<![CDATA[";
	static const char lua_footer[] = "]]>\x0E\x08\x0E\x05\x0E\x02\x0E\x00";
	static const char xml_header[] =
		"\x54\x49\x58\x43\x30\x31\x30\x30\x2D\x31\x2E\x30\x3F\x3E";

	int infile_is_xml = has_ext(inf_path, ".xml");
	int infile_is_lua = has_ext(inf_path, ".lua") || !strcmp("-", inf_path);

	*filename = gnu_basename(inf_path);
	if (infile_is_lua)
		*filename = "Problem1.xml";

	const char *header;
	size_t header_size;
	size_t footer_size;
	if (infile_is_xml) {
		header = xml_header;
		header_size = sizeof(xml_header) - 1;
		footer_size = 0;
	} else if (infile_is_lua) {
		header = lua_header;
		header_size = sizeof(lua_header) - 1;
		footer_size = sizeof(lua_footer) - 1;
	}
	else {
		header = NULL;
		header_size = 0;
		footer_size = 0;
	}

	FILE *inf;
	if (!strcmp(inf_path, "-"))
		inf = stdin;
	else
		inf = fopen(inf_path, "rb");
	if (!inf) {
		printf("can't open input file '%s'\n", inf_path);
		return NULL;
	}
	#define FREAD_BLOCK_SIZE 1024
	*obuf_size = header_size + FREAD_BLOCK_SIZE + footer_size;
	char *in_buf = malloc(*obuf_size);
	if (!in_buf) {
		puts("can't realloc in_buf");
		fclose(inf);
		return NULL;
	}
	memcpy(in_buf, header, header_size);
	size_t in_offset = header_size;
	while(1) {
		size_t read_size;
		if ((read_size = fread(in_buf + in_offset, 1, FREAD_BLOCK_SIZE, inf)) != FREAD_BLOCK_SIZE) {
			char *resized_in_buf;
			*obuf_size -= FREAD_BLOCK_SIZE - read_size;
			resized_in_buf = realloc(in_buf, *obuf_size);
			if (!resized_in_buf) {
				puts("can't realloc in_buf");
				free(in_buf);
				fclose(inf);
				return NULL;
			}
			in_buf = resized_in_buf;
			break;
		}
		char *resized_in_buf;
		*obuf_size += FREAD_BLOCK_SIZE;
		resized_in_buf = realloc(in_buf, *obuf_size);
		if (!resized_in_buf) {
			puts("can't realloc in_buf");
			free(in_buf);
			fclose(inf);
			return NULL;
		}
		in_buf = resized_in_buf;
		in_offset += read_size;
	}
	size_t in_size = *obuf_size - header_size - footer_size;
	fclose(inf);

	if (!infile_is_xml && !infile_is_lua) {
		return in_buf;
	}

	if (infile_is_xml) {
		in_buf = escape_unicode(in_buf, header_size, footer_size, in_size, obuf_size);
		if (!in_buf) return NULL;
		return reformat_xml_doc(in_buf, header_size, in_size, obuf_size, inf_path);
	} else {
		if (!(in_buf = fix_cdata_end_seq(in_buf, header_size, in_size, obuf_size)))
			return NULL;
		in_size = *obuf_size - header_size - footer_size;
		memcpy(in_buf + header_size + in_size, lua_footer, sizeof(lua_footer) - 1);
		return in_buf;
	}
}

int doccrypt(uint8_t *inout, long in_size) {
	unsigned i;
	DES_key_schedule ks1, ks2, ks3;
	DES_cblock cbc_data;
	/* Compatible with tien_crypted_header below from which they are derived */
	static unsigned char cbc1_key[8] = {0x16, 0xA7, 0xA7, 0x32, 0x68, 0xA7, 0xBA, 0x73};
	static unsigned char cbc2_key[8] = {0xD9, 0xA8, 0x86, 0xA4, 0x34, 0x45, 0x94, 0x10};
	static unsigned char cbc3_key[8] = {0x3D, 0x80, 0x8C, 0xB5, 0xDF, 0xB3, 0x80, 0x6B};
	unsigned char ivec[8] = {0x00, 0x00, 0x00, 0x00}; /* the last 4 bytes are incremented each time, LSB first */
	unsigned ivec_incr = 0;
	/* As stored in tien_crypted_header below */
	#define IVEC_BASE 0x6fe21307

	DES_set_key((DES_cblock*)&cbc1_key, &ks1);
	DES_set_key((DES_cblock*)&cbc2_key, &ks2);
	DES_set_key((DES_cblock*)&cbc3_key, &ks3);

	do {
		unsigned current_ivec = IVEC_BASE + ivec_incr++;
		if (ivec_incr == 1024)
			ivec_incr = 0;
		ivec[4] = (unsigned char)(current_ivec >> 0);
		ivec[5] = (unsigned char)(current_ivec >> 8);
		ivec[6] = (unsigned char)(current_ivec >> 16);
		ivec[7] = (unsigned char)(current_ivec >> 24);
		memcpy(&cbc_data, ivec, sizeof(DES_cblock));
		DES_ecb3_encrypt(&cbc_data, &cbc_data, &ks1, &ks2, &ks3, DES_ENCRYPT);
 		for (i = 0; i < ((unsigned)in_size >= sizeof(DES_cblock) ? sizeof(DES_cblock) : (unsigned)in_size); i++) {
			*inout++ ^= cbc_data.bytes[i];
		}
		in_size -= sizeof(DES_cblock);
	} while (in_size > 0);
	return 0;
}

static zipFile zipF = 0;

// stateful, keep the zipFile opened
int add_processed_file_to_tns(const char *infile_name, void const *in_buf, long in_size, const char *outfile_path, unsigned tiversion) {
	zip_fileinfo zi;
	if (!zipF && !(zipF = zipOpen(outfile_path, 0))) {
		puts("can't open zip-TNS file for writing");
		return 1;
	}

	zi.tmz_date.tm_sec = zi.tmz_date.tm_min = zi.tmz_date.tm_hour = 0;
	zi.tmz_date.tm_mday = zi.tmz_date.tm_mon = zi.tmz_date.tm_year = 0;
	zi.dosDate = 0; zi.internal_fa = 0; zi.external_fa = 0;
	int method = 0xD; // TI encrypted
	int level = 0;
	if (!has_ext(infile_name, ".xml")) {
		method = Z_DEFLATED; // just deflated
		level = Z_DEFAULT_COMPRESSION;
	}
	if (zipOpenNewFileInZip2(zipF, infile_name, &zi, NULL, 0, NULL, 0, NULL, method, level, 0, tiversion) != ZIP_OK) {
		puts("can't open file in zip-TNS file for writing");
close_quit:
		zipClose(zipF, NULL);
unlink_quit:
		unlink_path(outfile_path);
		return 1;
	}
	if (zipWriteInFileInZip(zipF, in_buf, in_size) != ZIP_OK) {
		puts("can't write file in zip-TNS file");
		goto close_quit;
	}
	if (zipCloseFileInZip(zipF) != ZIP_OK) {
		puts("can't close file in zip-TNS file");
		goto unlink_quit;
	}
	return 0;
}

int close_tns(const char *outfile_path) {
	if (zipClose(zipF, NULL) != ZIP_OK) {
		puts("can't close file in zip-TNS file");
		unlink_path(outfile_path);
		return 1;
	}
	return 0;
}

// returns the deflated size
long deflate_compressed_xml(void *def_buf, size_t def_size, const void *xmlc_buf, size_t xmlc_buf_size) {
	z_stream zstream;
	zstream.next_in = (Bytef*)xmlc_buf;
	zstream.next_out = (Bytef*)def_buf;
	zstream.avail_in = xmlc_buf_size;
	zstream.avail_out = def_size;
	zstream.zalloc = Z_NULL;
	zstream.zfree  = Z_NULL;
	/* -windowBits=-15: no zlib header */
	if (deflateInit2(&zstream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY)) {
		puts("can't deflateInit2");
		exit(1);
	}
	if (deflate(&zstream, Z_FINISH) != Z_STREAM_END) {
		puts("can't deflate");
		exit(1);
	}
	if (deflateEnd(&zstream)) {
		puts("can't deflateEnd");
		exit(1);
	}
	return zstream.total_out;
}

int add_default_document_to_tns(const char *tnsfile_path, unsigned tiversion) {
	static const char default_processed_document_xml[] =
		"\x0F\xCE\xD8\xD2\x81\x06\x86\x5B\x4A\x4A\xC5\xCE\xA9\x16\xF2\xD5\x1D\xA8\x2F\x6E"
		"\x00\x22\xF2\xF0\xC1\xA6\x06\x77\x4D\x7E\xA6\xC0\x3A\xF0\x5C\x74\xBA\xAA\x44\x60"
		"\xCD\x58\xE6\x70\xD7\x40\xF6\x9C\x17\xDC\xF0\x94\x77\xBF\xCA\xDE\xF7\x02\x09\xC9"
		"\x62\xB1\x5D\xEF\x22\xFA\x51\x37\xA0\x81\x91\x48\xE1\x83\x4D\xAD\x08\x31\x2D\xD0"
		"\xD3\xE3\x2D\x60\xAB\x13\xC2\x98\x2B\xED\x39\x5B\x09\x24\x39\x92\x2F\x0C\x7A\x4C"
		"\x95\x74\x91\x3B\x0C\xF4\x60\xCC\x73\x27\xCB\x07\x7E\x7F\xA9\x17\x87\xE2\xAC\xA2"
		"\x3B\xCC\xA0\xC4\xE3\x8E\x89\xF0\xC0\x51\x9F\xC2\xBE\xCE\x28\x45\xC3\xD4\x11\x90"
		"\xA6\xEC\x53\xA0\xFB\x5B\x46\x6B\x41\xAD\xE9\x53\xBB\x97\xDB\xB1\xD2\x68\xE2\xF6"
		"\x36\x0F\x26\x36\x75\x9B\xE9\x1F\x48\xAD\xE9\x29\x67\x00\x58\x19\xC3\xC0\x12\x76"
		"\xA0\x4A\x73\xF3\xB1\xD3\x09\x18\xD6\x06\xDD\x97\x24\x53\x3E\x22\xA4\xFB\x82\x50"
		"\x7B\x7C\x12\x88\x4E\x7D\x41\x80\xFE\x72\x92\x29\x87\xE8\x5C\x56\x72\xFF\x29\x16"
		"\x8C\x42\x5B\x8B\x9B\xA7\xD2\x08\x6D\xD3\x98\xFF\x91\xA9\x9E\xF3\x93\xA8\x2E\x1C"
		"\xB2\xA9\x6B\x6A\xDF\xF6\xCE\x2D\x15\x17\xCE\x6E\xC0\x4F\x9A\x9C\x0E\xDF\x19\x8D"
		"\x2D\xFA\x69\x9F\x11\xD2\x20\x12\xE0\x79\x14\x04\x4E\x62\x8F\x0A\x2A\x18\x72\x5A"
		"\x8B\x80\xB3\x3C\x9B\xD5\x67\x59\x4B\x51\x4D\xE0\xC3\x38\x28\xC3\xDC\xCD\x39\x22"
		"\x12\x8C\x40\x55";
	return add_processed_file_to_tns("Document.xml", default_processed_document_xml, sizeof(default_processed_document_xml) - 1, tnsfile_path, tiversion);
}

int add_compressed_xml_to_tns(const char *tnsfile_path, const char *filename, const void *xmlc_buf, size_t xmlc_buf_size, unsigned tiversion) {
	static const char tien_crypted_header[] =
		"\x0F\xCE\xD8\xD2\x81\x06\x86\x5B\x99\xDD\xA2\x3D\xD9\xE9\x4B\xD4\x31\xBB\x50\xB6"
		"\x4D\xB3\x29\x24\x70\x60\x49\x38\x1C\x30\xF8\x99\x00\x4B\x92\x64\xE4\x58\xE6\xBC";

	/* As expected by zlib */
	size_t def_size = (size_t) (xmlc_buf_size + (xmlc_buf_size * 0.1) + 12);
	size_t header_size = sizeof(tien_crypted_header) - 1;
	uint8_t *header_and_deflated_buf = malloc(def_size + header_size);
	if (!header_and_deflated_buf) {
		puts("can't malloc header_and_deflated_buf");
add_compressed_xml_err:
		free(header_and_deflated_buf);
		return 1;
	}

	uint8_t *def_buf = header_and_deflated_buf + header_size;
	long deflated_size = deflate_compressed_xml(def_buf, def_size, xmlc_buf, xmlc_buf_size);
	if (doccrypt(def_buf, deflated_size))
		goto add_compressed_xml_err;
	memcpy(header_and_deflated_buf, tien_crypted_header, header_size);
	if (add_processed_file_to_tns(filename, header_and_deflated_buf, header_size + deflated_size, tnsfile_path, tiversion))
		goto add_compressed_xml_err;
	free(header_and_deflated_buf);
	return 0;
}

int add_infile_to_tns(const char *infile_path, const char *tnsfile_path, unsigned tiversion) {
	size_t xmlc_buf_size;
	const char *filename = NULL;
	void *xmlc_buf = read_file_and_xml_compress(infile_path, &xmlc_buf_size, &filename);
	int ret;
	if (!xmlc_buf)
		return 1;

	if (has_ext(filename, ".xml")) // Only .xml files are encrypted
		ret = add_compressed_xml_to_tns(tnsfile_path, filename, xmlc_buf, xmlc_buf_size, tiversion);
	else // don't crypt, don't deflate: will be deflated by minizip
		ret = add_processed_file_to_tns(filename, xmlc_buf, xmlc_buf_size, tnsfile_path, tiversion);
	free(xmlc_buf);
	return ret;
}

// Add XML that refers to the Python script. The Python script itself is added separately.
int add_python_xml_to_tns(const char *python_path, const char *tnsfile_path, unsigned tiversion) {
	static const char py_header[] =
		"TIXC0100-1.0?><prob xmlns=\"urn:TI.Problem\" ver=\"1.0\" pbname=\"\">"
		"<sym>\x0E\x01<card clay=\"0\" h1=\"10000\" h2=\"10000\" w1=\"10000\" "
		"w2=\"10000\"><isDummyCard>0\x0E\x03<flag>0\x0E\x04<wdgt xmlns:py=\"urn:"
		"TI.PythonEditor\" type=\"TI.PythonEditor\" ver=\"1.0\"><py:data><py:name>";
	static const char py_footer[] =
		"\x0E\x07<py:dirf>-10000000\x0E\x08\x0E\x06<py:mFlags>1024\x0E\x09"
		"<py:value>10\x0E\x0A\x0E\x05\x0E\x02\x0E\x00";

	const char *filename = gnu_basename(python_path);
	size_t filename_len = strlen(filename);
	if (filename_len > 240) {
		puts("Python script filenames limited to 240 characters");
		return 1;
	}

	size_t encoded_filename_size;
	char *encoded_filename = encode_python_name_text(filename, &encoded_filename_size);
	if (!encoded_filename)
		return 1;

	size_t total_size = (sizeof(py_header) - 1) + encoded_filename_size + (sizeof(py_footer) - 1);
	char *xmlc_buf = malloc(total_size);
	if (!xmlc_buf) {
		puts("can't malloc xmlc_buf");
		free(encoded_filename);
		return 1;
	}

	memcpy(xmlc_buf, py_header, sizeof(py_header) - 1);
	memcpy(xmlc_buf + sizeof(py_header) - 1, encoded_filename, encoded_filename_size);
	memcpy(xmlc_buf + sizeof(py_header) - 1 + encoded_filename_size, py_footer, sizeof(py_footer) - 1);

	int ret = add_compressed_xml_to_tns(tnsfile_path, "Problem1.xml", xmlc_buf, total_size, tiversion);
	free(xmlc_buf);
	free(encoded_filename);
	return ret;
}

int convert_inputs_to_tns(int infile_count, char *const infiles[], const char *outfile_path) {
	if (infile_count <= 0) {
		puts("no input files were provided");
		return 1;
	}

	char *derived_outfile_path = NULL;
	char *temp_outfile_path = NULL;
	const char *final_outfile_path = outfile_path;
	if (infile_count == 1) {
		derived_outfile_path = resolve_single_infile_outfile_path(infiles[0], outfile_path);
		if (!derived_outfile_path)
			return 1;
		final_outfile_path = derived_outfile_path;
	}
	else if (!outfile_path) {
		puts("multiple input files require an explicit output path");
		return 1;
	}

	temp_outfile_path = create_temp_outfile_path(final_outfile_path);
	if (!temp_outfile_path) {
		free(derived_outfile_path);
		return 1;
	}
	unsigned tiversion = 0x0500; // default to document version 5
	for (int i = 0; i < infile_count; i++) {
		if (has_ext(infiles[i], ".bmp")) {
			tiversion = 0x0700; // bitmap files require the document type to be bumped up to 7
			break;
		}
	}

	zipF = 0; // Only useful for emscripten (possible multiple main() calls)

	// Document.xml must be added first to the TNS
	int has_processed_documentxml = 0;
	for (int i = 0; i < infile_count; i++) {
		if (!strcmp("Document.xml", gnu_basename(infiles[i]))) {
			printf("processing '%s'...\n", infiles[i]);
			int ret = add_infile_to_tns(infiles[i], temp_outfile_path, tiversion);
			if (ret) {
				goto convert_cleanup;
			}
			has_processed_documentxml = 1;
		}
	}
	if (!has_processed_documentxml) {
		int ret = add_default_document_to_tns(temp_outfile_path, tiversion);
		if (ret) {
			goto convert_cleanup;
		}
	}

	// Then add all the other files
	int is_converting_lua = 0;
	int added_python_xml = 0;
	int ret;
	for (int i = 0; i < infile_count; i++) {
		if (!strcmp("Document.xml", gnu_basename(infiles[i])))
			continue;
		printf("processing '%s'...\n", infiles[i]);
		if (has_ext(infiles[i], ".lua") || !strcmp("-", infiles[i])) {
			if (is_converting_lua) {
				puts("[WARN] skipping it, can only add a single Lua script to the TNS file");
				continue;
			}
			is_converting_lua = 1;
		}
		if (!added_python_xml && has_ext(infiles[i], ".py")) { // Add XML for just the first Python file
			ret = add_python_xml_to_tns(infiles[i], temp_outfile_path, tiversion);
			if (ret) {
				goto convert_cleanup;
			}
			added_python_xml = 1;
		}
		ret = add_infile_to_tns(infiles[i], temp_outfile_path, tiversion);
		if (ret) {
			goto convert_cleanup;
		}
	}

	if (close_tns(temp_outfile_path))
		goto convert_cleanup;
	if (replace_file_path(temp_outfile_path, final_outfile_path)) {
		printf("can't rename temporary output file to '%s'\n", final_outfile_path);
		unlink_path(temp_outfile_path);
		goto convert_cleanup;
	}
	printf("wrote '%s'\n", final_outfile_path);
	free(temp_outfile_path);
	free(derived_outfile_path);
	return 0;

convert_cleanup:
	unlink_path(temp_outfile_path);
	free(temp_outfile_path);
	free(derived_outfile_path);
	return 1;
}

int convert_python_directory_recursive(const char *dirpath, recursive_batch_stats *stats) {
	directory_entry_list entries = {0};
	if (collect_directory_entries(dirpath, &entries)) {
		stats->failed++;
		return 1;
	}

	int status = 0;
	for (size_t i = 0; i < entries.count; i++) {
		char *entry_path = join_filepath(dirpath, entries.names[i]);
		if (!entry_path) {
			stats->failed++;
			status = 1;
			continue;
		}

		path_info info;
		if (get_path_info(entry_path, &info)) {
			printf("can't stat '%s'\n", entry_path);
			stats->failed++;
			status = 1;
			free(entry_path);
			continue;
		}

		if (info.is_symlink) {
			printf("[SKIP] skipping symlink '%s'\n", entry_path);
			stats->skipped++;
			free(entry_path);
			continue;
		}

		if (info.is_directory) {
			if (convert_python_directory_recursive(entry_path, stats))
				status = 1;
		}
		else if (has_ext(entry_path, ".py")) {
			char *infiles[] = { entry_path };
			if (convert_inputs_to_tns(1, infiles, NULL)) {
				stats->failed++;
				status = 1;
			}
			else
				stats->converted++;
		}
		else
			stats->skipped++;

		free(entry_path);
	}

	free_directory_entries(&entries);
	return status;
}

int convert_recursive_python_inputs(int argc, char *argv[]) {
	recursive_batch_stats stats = {0};
	int status = 0;

	for (int i = 1; i < argc; i++) {
		if (path_is_directory(argv[i])) {
			if (convert_python_directory_recursive(argv[i], &stats))
				status = 1;
			continue;
		}

		if (!has_ext(argv[i], ".py")) {
			printf("recursive mode only supports Python files and directories: '%s'\n", argv[i]);
			stats.failed++;
			status = 1;
			continue;
		}

		char *infiles[] = { argv[i] };
		if (convert_inputs_to_tns(1, infiles, NULL)) {
			stats.failed++;
			status = 1;
		}
		else
			stats.converted++;
	}

	print_recursive_summary(&stats);
	if (!stats.converted) {
		puts("no Python files found to convert");
		return 1;
	}

	return status;
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		print_usage();
		return 0;
	}
	if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
		print_usage();
		return 0;
	}
	if (argc == 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
		puts("Luna v" LUNA_VER);
		return 0;
	}

	for (int i = 1; i < argc; i++) {
		if (path_is_directory(argv[i])) {
			if (argc > 2 && !path_is_directory(argv[argc - 1]) && has_ext(argv[argc - 1], ".tns")) {
				puts("recursive directory mode does not accept an explicit output path");
				return 1;
			}
			return convert_recursive_python_inputs(argc, argv);
		}
	}

	if (argc == 2)
		return convert_inputs_to_tns(1, &argv[1], NULL);
	if (argc == 3)
		return convert_inputs_to_tns(1, &argv[1], argv[2]);
	return convert_inputs_to_tns(argc - 2, &argv[1], argv[argc - 1]);
}
