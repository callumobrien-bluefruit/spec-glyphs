#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include "glib-2.0/gmodule.h"
#include "unicode/utf8.h"
#include "libxml/parser.h"
#include "libxml/xmlmemory.h"
#pragma clang diagnostic pop

#define FONT_COUNT 3

static void die(const char *msg) __attribute__ ((noreturn));
static GHashTable *get_font_usages(const char *screens_path);
static GHashTable **determine_characters(GHashTable *font_usages,
                                     const char *translations_path);
static bool read_screen(GHashTable *usages, xmlNode *screen);
static bool read_translations(GHashTable *font_usages,
					          xmlDoc *doc,
					          const xmlNode *text,
					          GHashTable *characters[FONT_COUNT]);
static bool read_translation(GHashTable *font_usages,
					         const xmlChar *text_id,
					         const xmlChar *trans_text,
					         GHashTable *characters[FONT_COUNT]);
static void print_glyph(gpointer key, gpointer value, gpointer user_data);

int main(int argc, char *argv[])
{
	GHashTable *font_usages, **characters;

	if (argc < 3)
		die("usage: spec-glyphs SCREENS_XML TRANSLATIONS_XML");

	font_usages = get_font_usages(argv[1]);
	if (!font_usages)
		die("failed to process screens XML");

	characters = determine_characters(font_usages, argv[2]);
	if (!characters)
		die("failed to process translations XML");

	for (unsigned i = 0; i < FONT_COUNT; ++i) {
		printf("FONT%d:", i);
		g_hash_table_foreach(characters[i], print_glyph, NULL);
		printf("\n");
	}

	for (unsigned i = 0; i < FONT_COUNT; ++i)
		g_hash_table_destroy(characters[i]);
	g_hash_table_destroy(font_usages);
	return 0;
}

/// Prints `msg` to `stderr` then exits with status 1
void die(const char *msg)
{
	fprintf(stderr, "spec-characters: %s\n", msg);
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

	xmlChar *screenName = xmlGetProp(screen, (const xmlChar *)"name");
	xmlFree(screenName);

	for (text = screen->xmlChildrenNode; text; text = text->next) {
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
/// are used in each font, returning an array of `FONT_COUNT` hash
/// tables with an entry for each used glyph.  Returns `NULL` on
/// failure.
GHashTable **determine_characters(GHashTable *font_usages,
                              const char *translations_path)
{
	xmlDoc *doc;
	xmlNode *root, *text;
	GHashTable **characters;

	characters = calloc(FONT_COUNT, sizeof(GHashTable *));
	for (unsigned i = 0; i < FONT_COUNT; ++i)
		characters[i] = g_hash_table_new_full(g_int_hash, g_int_equal, free, NULL);

	doc = xmlParseFile(translations_path);
	if (!doc)
		return NULL;
	root = xmlDocGetRootElement(doc);
	if (!root)
		return NULL;

	for (text = root->xmlChildrenNode; text; text = text->next) {
		if (xmlStrcmp(text->name, (const xmlChar *)"trans-unit") != 0)
			continue;

		if (!read_translations(font_usages, doc, text, characters))
			return NULL;
	}

	xmlFreeDoc(doc);
	return characters;
}

/// Processes a single translation unit, adding characters used in its
/// translations to `characters` for any fonts it is used in. Returns `true`
/// on success, `false` on failure.
bool read_translations(GHashTable *font_usages,
                       xmlDoc *doc,
                       const xmlNode *text,
                       GHashTable *characters[FONT_COUNT])
{
	xmlNode *trans;
	xmlChar *text_id, *trans_text;

	text_id = xmlGetProp(text, (const xmlChar *)"name");
	if (!text_id)
		return false;

	for (trans = text->xmlChildrenNode; trans; trans = trans->next) {
		if (xmlStrcmp(trans->name, (const xmlChar *)"source") != 0
			&& xmlStrcmp(trans->name, (const xmlChar *)"target") != 0)
			continue;

		trans_text = xmlNodeListGetString(doc, trans->xmlChildrenNode, 1);
		if (!trans_text)
			return false;
		if (!read_translation(font_usages, text_id, trans_text, characters))
			return false;

		xmlFree(trans_text);
	}

	xmlFree(text_id);
	return true;
}

/// Processes a single translation, adding its characters to `characters`
/// for any fonts its parent translation unit is used in. Returns `true`
/// on success, `false` on failure.
bool read_translation(GHashTable *font_usages,
					  const xmlChar *text_id,
					  const xmlChar *trans_text,
					  GHashTable *characters[FONT_COUNT])
{
	long i, c, *p;
	const xmlChar *s;
	bool *font_usage;

	font_usage = (bool *)g_hash_table_lookup(font_usages, text_id);
	if (!font_usage)
		return false;

	for (i = 0, s = trans_text; *s != '\0'; s += i, i = 0) {
		U8_NEXT(s, i, -1, c)
		if (c < 0)
			return false;

		for (unsigned j = 0; j < FONT_COUNT; ++j) {
			if (font_usage[j]) {
				p = malloc(sizeof(long));
				*p = c;
				g_hash_table_add(characters[j], p);
			}
		}
	}

	return true;
}

void print_glyph(gpointer key, gpointer value, gpointer user_data)
{
	(void)value;
	(void)user_data;
	printf(" %ld", *(long *)key);
}
