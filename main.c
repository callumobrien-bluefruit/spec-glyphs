#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include "glib-2.0/gmodule.h"
#include "libxml/parser.h"
#include "libxml/xmlmemory.h"
#include "unicode/utf8.h"
#pragma clang diagnostic pop

#define FONT_COUNT 3
#define MAX_PATH_LEN 128
#define MAX_SPEC_LEN 512
#ifndef WINDOWS
#define PATH_SEP "/"
#else
#define PATH_SEP "\\"
#endif

struct font {
	char path[MAX_PATH_LEN];
	unsigned size, width, height, x, y;
};

static void die(const char *msg) __attribute__((noreturn));
static GHashTable *get_font_usages(const char *screens_path);
static GHashTable **determine_chars(GHashTable *font_usages,
                                    const char *translations_path);
static bool read_screen(GHashTable *usages, xmlNode *screen);
static bool read_translations(GHashTable *font_usages,
                              xmlDoc *doc,
                              const xmlNode *text,
                              GHashTable *chars[FONT_COUNT]);
static bool read_translation(bool *font_usage,
                             const xmlChar *trans_text,
                             GHashTable *chars[FONT_COUNT]);
static struct font *read_fonts(const char *phys_attrib_xml);
static int extract_font(const xmlNode *fonts, struct font *out);
static bool extract_unsigned_prop(const xmlNode *node,
                                  const char *prop_name,
                                  unsigned *value_out);
static bool write_specs(GHashTable **chars,
                        const struct font *fonts,
                        const char *out_dir);
static void build_path(char *out,
                       unsigned n,
                       const char *dir,
                       const char *name);

int main(int argc, char *argv[])
{
	unsigned i;
	GHashTable *font_usages, **chars;
	struct font *fonts;

	if (argc < 5)
		die("usage: spec-glyphs SCREENS_XML TRANSLATIONS_XML PHYS_ATTRIB_XML "
		    "OUT_DIR");

	font_usages = get_font_usages(argv[1]);
	if (!font_usages)
		die("failed to process screens XML");

	chars = determine_chars(font_usages, argv[2]);
	if (!chars)
		die("failed to process translations XML");

	fonts = read_fonts(argv[3]);
	if (!fonts)
		die("failed to read font options from physical attributes XML");

	if (!write_specs(chars, fonts, argv[4]))
		die("failed to write glyphs specs");

	free(fonts);
	for (i = 0; i < FONT_COUNT; ++i)
		g_hash_table_destroy(chars[i]);
	g_hash_table_destroy(font_usages);
	return 0;
}

/// Prints `msg` to `stderr` then exits with status 1
void die(const char *msg)
{
	fprintf(stderr, "spec-glyphs: %s\n", msg);
	exit(1);
}

/// Builds a hash table mapping text IDs to an array of bools such that
/// for some text ID `t` and font index `f`, `t` is drawn in `f` if and
/// only if `g_hash_table_lookup(ht, t)[f]` is true (where ht is the
/// produced hash table). Returns `NULL` on error.
GHashTable *get_font_usages(const char *screens_path)
{
	xmlDoc *doc;
	xmlNode *root, *screen;
	GHashTable *usages;

	doc = xmlParseFile(screens_path);
	if (!doc)
		return NULL;
	root = xmlDocGetRootElement(doc);
	if (!root)
		return NULL;

	usages = g_hash_table_new_full(g_str_hash, g_str_equal, xmlFree, free);
	if (usages == NULL)
		return NULL;

	for (screen = root->xmlChildrenNode; screen; screen = screen->next) {
		if (xmlStrcmp(screen->name, (const xmlChar *)"screen") != 0)
			continue;

		if (!read_screen(usages, screen))
			return NULL;
	}

	xmlFreeDoc(doc);
	return usages;
}

/// Processes a single screen XML node, updating `usages` to account for
/// `screen`'s text items. Returns `true` on success, `false` on failure.
bool read_screen(GHashTable *usages, xmlNode *screen)
{
	xmlNode *text;
	xmlChar *text_id, *font_str;
	unsigned font;
	bool *usage;

	for (text = screen->xmlChildrenNode; text; text = text->next) {
		if (xmlStrcmp(text->name, (const xmlChar *)"variable_region") == 0) {
			if (!read_screen(usages, text))
				return false;
			continue;
		}

		if (xmlStrcmp(text->name, (const xmlChar *)"text") != 0)
			continue;

		// libxml gives children content nodes the name "text", so we
		// expect to see text nodes without a value attribute
		text_id = xmlGetProp(text, (const xmlChar *)"value");
		if (!text_id)
			continue;

		font_str = xmlGetProp(text, (const xmlChar *)"font");
		if (!font_str)
			return false;
		font = (unsigned)atoi((const char *)font_str);
		xmlFree(font_str);
		if (font >= FONT_COUNT)
			return false;

		usage = g_hash_table_lookup(usages, text_id);
		if (!usage) {
			usage = calloc(FONT_COUNT, sizeof(bool));
			g_hash_table_insert(usages, text_id, usage);
		}
		usage[font] = true;
	}

	return true;
}

/// Determines, from `font_usages` and translations XML, which characters
/// are used in each font, returning an array of `FONT_COUNT` hash tables
/// with an entry for each used glyph. Returns `NULL` on failure.
GHashTable **determine_chars(GHashTable *font_usages,
                             const char *translations_path)
{
	xmlDoc *doc;
	xmlNode *root, *text;
	GHashTable **chars;

	chars = calloc(FONT_COUNT, sizeof(GHashTable *));
	for (unsigned i = 0; i < FONT_COUNT; ++i)
		chars[i] = g_hash_table_new_full(g_int_hash, g_int_equal, free, NULL);

	doc = xmlParseFile(translations_path);
	if (!doc)
		return NULL;
	root = xmlDocGetRootElement(doc);
	if (!root)
		return NULL;

	for (text = root->xmlChildrenNode; text; text = text->next) {
		if (xmlStrcmp(text->name, (const xmlChar *)"trans-unit") != 0)
			continue;

		if (!read_translations(font_usages, doc, text, chars))
			return NULL;
	}

	xmlFreeDoc(doc);
	return chars;
}

/// Processes a single translation unit, adding characters used in its
/// translations to `chars` for any fonts it is used in. Returns `true` on
/// success, `false` on failure.
bool read_translations(GHashTable *font_usages,
                       xmlDoc *doc,
                       const xmlNode *text,
                       GHashTable **chars)
{
	xmlNode *trans;
	xmlChar *text_id, *trans_text;
	bool *font_usage;

	text_id = xmlGetProp(text, (const xmlChar *)"name");
	if (!text_id)
		return false;

	for (trans = text->xmlChildrenNode; trans; trans = trans->next) {
		if (xmlStrcmp(trans->name, (const xmlChar *)"source") != 0
		    && xmlStrcmp(trans->name, (const xmlChar *)"target") != 0)
			continue;

		trans_text = xmlNodeListGetString(doc, trans->xmlChildrenNode, 1);
		if (!trans_text) {
			fprintf(stderr,
			        "spec-glyphs: warning: empty translation for %s\n",
			        text_id);
			continue;
		}

		font_usage = (bool *)g_hash_table_lookup(font_usages, text_id);
		if (!font_usage) {
			fprintf(stderr,
			        "spec-glyphs: warning: unused translation %s\n",
			        text_id);
			return true;
		}

		if (!read_translation(font_usage, trans_text, chars))
			return false;

		xmlFree(trans_text);
	}

	xmlFree(text_id);
	return true;
}

/// Processes a single translation, adding its characters to `chars` for any
/// fonts its parent translation unit is used in. Returns `true` on success,
/// `false` on failure.
bool read_translation(bool *font_usage,
                      const xmlChar *trans_text,
                      GHashTable **chars)
{
	const xmlChar *s;
	int i, c;
	unsigned j, *p;

	for (i = 0, s = trans_text; *s != '\0'; s += i, i = 0) {
		U8_NEXT(s, i, -1, c)
		if (c < 0)
			return false;

		for (j = 0; j < FONT_COUNT; ++j) {
			if (font_usage[j]) {
				p = malloc(sizeof(unsigned));
				*p = (unsigned)c;
				g_hash_table_add(chars[j], p);
			}
		}
	}

	return true;
}

/// Read font options from `phys_attrib_path`, returning them in a
/// heap-allocated array on success, `NULL` on failure.
struct font *read_fonts(const char *phys_attrib_path)
{
	xmlDoc *doc;
	xmlNode *root, *xml_fonts, *xml_font;
	struct font font, *fonts;
	int index;

	doc = xmlParseFile(phys_attrib_path);
	if (!doc)
		return NULL;
	root = xmlDocGetRootElement(doc);
	if (!root)
		return NULL;

	fonts = calloc(FONT_COUNT, sizeof(struct font));
	for (xml_fonts = root->xmlChildrenNode; xml_fonts;
	     xml_fonts = xml_fonts->next) {
		if (xmlStrcmp(xml_fonts->name, (const xmlChar *)"Fonts") != 0)
			continue;

		for (xml_font = xml_fonts->xmlChildrenNode; xml_font;
		     xml_font = xml_font->next) {
			if (xmlStrcmp(xml_font->name, (const xmlChar *)"Font") != 0)
				continue;

			index = extract_font(xml_font, &font);
			if (index < 0 || index >= FONT_COUNT) {
				free(fonts);
				return NULL;
			}

			fonts[index] = font;
		}
	}

	return fonts;
}

/// Extracts font options from a XML Font node, writing them to `*out` and
/// returning its font index. Returns `-1` on error.
int extract_font(const xmlNode *font, struct font *out)
{
	char *s;
	int n, index;

	if (!extract_unsigned_prop(font, "Size", &out->size)
	    || !extract_unsigned_prop(font, "Width", &out->width)
	    || !extract_unsigned_prop(font, "Height", &out->height)
	    || !extract_unsigned_prop(font, "StartX", &out->x)
	    || !extract_unsigned_prop(font, "StartY", &out->y))
		return -1;

	// Can't use `extract_unsigned_prop` here because of the "FONT" prefix
	s = (char *)xmlGetProp(font, (const xmlChar *)"Name");
	if (!s)
		return -1;
	n = sscanf(s, "FONT%d", &index);
	xmlFree(s);
	if (n != 1)
		return -1;

	s = (char *)xmlGetProp(font, (const xmlChar *)"TrueTypeLib");
	if (!s)
		return -1;
	strncpy(out->path, s, MAX_PATH_LEN);
	xmlFree(s);

	return index;
}

/// Extracts the value of the property called `prop_name` from `node`,
/// converting it to an integer and writing it to `value_out`. Returns
/// `true` on success, `false` on error.
bool extract_unsigned_prop(const xmlNode *node,
                           const char *prop_name,
                           unsigned *value_out)
{
	int n;
	char *s;

	s = (char *)xmlGetProp(node, (const xmlChar *)prop_name);
	if (!s)
		return false;
	n = sscanf(s, "%u", value_out);

	xmlFree(s);
	return n == 1;
}

/// Writes a file in `out_dir` for each needed glyph detailing rendering
/// parameters as tab-seperated values. The filename is the MD5 hash of the
/// content and the file is only written if it does not already exist. Returns
/// `true` on success, `false` on error.
bool write_specs(GHashTable **chars,
                 const struct font *fonts,
                 const char *out_dir)
{
	GHashTableIter iter;
	unsigned i, *char_id;
	char path[MAX_PATH_LEN], spec[MAX_SPEC_LEN], *md5;
	FILE *fp;
	int n;

	for (i = 0; i < FONT_COUNT; ++i) {
		for (g_hash_table_iter_init(&iter, chars[i]);
		     g_hash_table_iter_next(&iter, (void **)&char_id, NULL);) {
			n = snprintf(spec,
			             MAX_SPEC_LEN,
			             "%d\t%s\t%d\t%d\t%d\t%d\t%d\n",
			             *char_id,
			             fonts[i].path,
			             fonts[i].size,
			             fonts[i].width,
			             fonts[i].height,
			             fonts[i].x,
			             fonts[i].y);
			if (n >= MAX_SPEC_LEN)
				return NULL;

			md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, spec, -1);
			build_path(path, MAX_PATH_LEN, out_dir, md5);
			g_free(md5);

			if ((fp = fopen(path, "r")) != NULL) {
				// File exists
				fclose(fp);
				continue;
			}

			fp = fopen(path, "w");
			if (fp == NULL || fputs(spec, fp) == EOF)
				return false;
			fclose(fp);
		}
	}

	return true;
}

/// Builds a filesystem path with directory part `dir` and filename `name`
void build_path(char *out, unsigned n, const char *dir, const char *name)
{
	strncpy(out, dir, n);
	strncat(out, PATH_SEP, n);
	strncat(out, name, n);
}
