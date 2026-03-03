/*
	XML Parser using yxml — a tiny zero-allocation pull parser.
	Replaces the previous libxml2 xmlTextReader implementation.

	Each parse function returns 1 on success, 0 on failure.
	On failure tmx_errno is set and an error message is generated.
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "yxml.h"
#include "tmx.h"
#include "tmx_utils.h"

/*
	Input abstraction — all load variants feed into a single buffer
	that yxml then processes byte-by-byte.
*/
typedef struct {
	char* buf;      /* owned, malloc'd document buffer */
	size_t      buf_len;
	int         owned;    /* 1 if buf should be freed */
} xml_reader;

static int reader_open_file(xml_reader* r, const char* path) {
	FILE* f;
	long len;
	memset(r, 0, sizeof(*r));
	f = fopen(path, "rb");
	if (!f) return 0;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len < 0) { fclose(f); return 0; }
	r->buf = (char*)tmx_alloc_func(NULL, (size_t)len + 1);
	if (!r->buf) { fclose(f); return 0; }
	if ((long)fread(r->buf, 1, (size_t)len, f) != len) {
		tmx_free_func(r->buf);
		r->buf = NULL;
		fclose(f);
		return 0;
	}
	r->buf[len] = '\0';
	r->buf_len = (size_t)len;
	r->owned = 1;
	fclose(f);
	return 1;
}

static int reader_open_fd(xml_reader* r, int fd) {
	size_t cap = 4096, total = 0;
	int n;
	char tmp[4096];
	memset(r, 0, sizeof(*r));
	r->buf = (char*)tmx_alloc_func(NULL, cap);
	if (!r->buf) return 0;
	while ((n = (int)read(fd, tmp, sizeof(tmp))) > 0) {
		if (total + (size_t)n >= cap) {
			cap = (total + (size_t)n) * 2;
			r->buf = (char*)tmx_alloc_func(r->buf, cap);
			if (!r->buf) return 0;
		}
		memcpy(r->buf + total, tmp, (size_t)n);
		total += (size_t)n;
	}
	r->buf[total] = '\0';
	r->buf_len = total;
	r->owned = 1;
	return 1;
}

static int reader_open_callback(xml_reader* r, tmx_read_functor cb, void* ud) {
	size_t cap = 4096, total = 0;
	int n;
	char tmp[4096];
	memset(r, 0, sizeof(*r));
	r->buf = (char*)tmx_alloc_func(NULL, cap);
	if (!r->buf) return 0;
	while ((n = cb(ud, tmp, (int)sizeof(tmp))) > 0) {
		if (total + (size_t)n >= cap) {
			cap = (total + (size_t)n) * 2;
			r->buf = (char*)tmx_alloc_func(r->buf, cap);
			if (!r->buf) return 0;
		}
		memcpy(r->buf + total, tmp, (size_t)n);
		total += (size_t)n;
	}
	r->buf[total] = '\0';
	r->buf_len = total;
	r->owned = 1;
	return 1;
}

static void reader_close(xml_reader* r) {
	if (r->owned && r->buf) {
		tmx_free_func(r->buf);
	}
	r->buf = NULL;
}

/*
	Attribute accumulation — yxml delivers attribute names and values
	one character at a time. We accumulate them into name/value pairs.
*/
#define MAX_ATTRS 64

typedef struct {
	char* name;
	char* value;
} xml_attr;

typedef struct {
	xml_attr attrs[MAX_ATTRS];
	int      attr_count;
	/* Current attribute being accumulated */
	char* attr_name;
	size_t   attr_name_len;
	size_t   attr_name_cap;
	char* attr_value;
	size_t   attr_value_len;
	size_t   attr_value_cap;
	/* Content accumulator */
	char* content;
	size_t   content_len;
	size_t   content_cap;
	/* Event state */
	char     elem_name[256]; /* element name saved from YXML_ELEMSTART */
	int      in_attrs;       /* 1 while accumulating attributes */
	int      pending_end;    /* 1 when self-closing elem owes an EVT_ELEM_END */
	int      pending_start;  /* 1 when a new ELEMSTART arrived while in_attrs;
								parent ELEM_START was returned, now deliver child */
	char     pending_elem[256]; /* saved child element name for pending_start */
	int      pending_content; /* 1 when had_content and an ELEMEND arrived */
} xml_accum;

static void accum_init(xml_accum* a) {
	memset(a, 0, sizeof(*a));
}

static void accum_free_attrs(xml_accum* a) {
	int i;
	for (i = 0; i < a->attr_count; i++) {
		tmx_free_func(a->attrs[i].name);
		tmx_free_func(a->attrs[i].value);
	}
	a->attr_count = 0;
}

static void accum_destroy(xml_accum* a) {
	accum_free_attrs(a);
	if (a->attr_name) tmx_free_func(a->attr_name);
	if (a->attr_value) tmx_free_func(a->attr_value);
	if (a->content) tmx_free_func(a->content);
	memset(a, 0, sizeof(*a));
}

static void accum_attr_start(xml_accum* a) {
	a->attr_name_len = 0;
	a->attr_value_len = 0;
}

static void accum_attr_name_append(xml_accum* a, const char* s) {
	size_t slen = strlen(s);
	if (a->attr_name_len + slen >= a->attr_name_cap) {
		a->attr_name_cap = (a->attr_name_len + slen + 1) * 2;
		a->attr_name = (char*)tmx_alloc_func(a->attr_name, a->attr_name_cap);
	}
	memcpy(a->attr_name + a->attr_name_len, s, slen);
	a->attr_name_len += slen;
	a->attr_name[a->attr_name_len] = '\0';
}

static void accum_attr_value_append(xml_accum* a, const char* s) {
	size_t slen = strlen(s);
	if (a->attr_value_len + slen >= a->attr_value_cap) {
		a->attr_value_cap = (a->attr_value_len + slen + 1) * 2;
		a->attr_value = (char*)tmx_alloc_func(a->attr_value, a->attr_value_cap);
	}
	memcpy(a->attr_value + a->attr_value_len, s, slen);
	a->attr_value_len += slen;
	a->attr_value[a->attr_value_len] = '\0';
}

static void accum_attr_finish(xml_accum* a) {
	if (a->attr_count < MAX_ATTRS) {
		a->attrs[a->attr_count].name = tmx_strdup(a->attr_name);
		a->attrs[a->attr_count].value = tmx_strdup(a->attr_value);
		a->attr_count++;
	}
}

static void accum_content_reset(xml_accum* a) {
	a->content_len = 0;
	if (a->content) a->content[0] = '\0';
}

static void accum_content_append(xml_accum* a, const char* s) {
	size_t slen = strlen(s);
	if (a->content_len + slen >= a->content_cap) {
		a->content_cap = (a->content_len + slen + 1) * 2;
		if (a->content_cap < 256) a->content_cap = 256;
		a->content = (char*)tmx_alloc_func(a->content, a->content_cap);
	}
	memcpy(a->content + a->content_len, s, slen);
	a->content_len += slen;
	a->content[a->content_len] = '\0';
}

/* Lookup an attribute by name; returns NULL if not found */
static const char* get_attr(xml_accum* a, const char* name) {
	int i;
	for (i = 0; i < a->attr_count; i++) {
		if (!strcmp(a->attrs[i].name, name)) {
			return a->attrs[i].value;
		}
	}
	return NULL;
}

/*
	yxml event-driven recursive descent parser.
	Each element handler reads its own attributes and children,
	advancing the yxml cursor until its closing tag.
*/

/* Forward declarations */
static int parse_properties_y(yxml_t* x, const char* buf, size_t len, size_t* pos, xml_accum* a, int* depth, tmx_properties** prop_hashptr);
static tmx_template* parse_template_document_y(const char* buf, size_t len, tmx_resource_manager* rc_mgr, const char* filename);

/*
	Event types returned by next_event()
*/
enum xml_event {
	EVT_ELEM_START,   /* element opened, attrs accumulated */
	EVT_ELEM_END,     /* element closed */
	EVT_CONTENT,      /* text content accumulated (check a->content) */
	EVT_EOF,          /* end of document */
	EVT_ERROR         /* parse error */
};

/*
	Advance yxml to the next meaningful event.
	On EVT_ELEM_START: accum_elem_name has the element name, attrs are in accum.
	On EVT_ELEM_END: depth has been decremented.
	On EVT_CONTENT: a->content has accumulated text.
	*depth tracks nesting depth.

	State machine:
	  in_attrs  == 1 while accumulating attributes after YXML_ELEMSTART.
	  pending_end == 1 when a self-closing element finished its attributes
					  via YXML_ELEMEND; we returned EVT_ELEM_START and owe
					  an EVT_ELEM_END on the next call.
*/
static enum xml_event next_event(yxml_t* x, const char* buf, size_t len, size_t* pos, xml_accum* a, int* depth) {
	yxml_ret_t r;
	int had_content = 0;

	/* Deliver pending events from previous call */
	if (a->pending_end) {
		a->pending_end = 0;
		(*depth)--;
		return EVT_ELEM_END;
	}
	if (a->pending_start) {
		a->pending_start = 0;
		accum_free_attrs(a);
		accum_content_reset(a);
		strncpy(a->elem_name, a->pending_elem, sizeof(a->elem_name) - 1);
		a->elem_name[sizeof(a->elem_name) - 1] = '\0';
		a->in_attrs = 1;
		/* Fall through to continue parsing this element's attrs */
	}
	if (a->pending_content) {
		a->pending_content = 0;
		(*depth)--;
		return EVT_ELEM_END;
	}

	while (*pos < len) {
		r = yxml_parse(x, buf[*pos]);
		if (r < YXML_OK) {
			tmx_err(E_XDATA, "xml parser: syntax error at byte %u", (unsigned)*pos);
			return EVT_ERROR;
		}
		(*pos)++;

		switch (r) {
		case YXML_ELEMSTART:
			if (a->in_attrs) {
				/* Previous element's attrs are done. Save new child element,
				   return parent's EVT_ELEM_START. Next call delivers child. */
				a->in_attrs = 0;
				(*depth)++;
				a->pending_start = 1;
				strncpy(a->pending_elem, x->elem, sizeof(a->pending_elem) - 1);
				a->pending_elem[sizeof(a->pending_elem) - 1] = '\0';
				return EVT_ELEM_START;
			}
			if (had_content) {
				/* New element starting, return accumulated content first.
				   Save the new element as pending. */
				(*depth)++;
				a->pending_start = 1;
				strncpy(a->pending_elem, x->elem, sizeof(a->pending_elem) - 1);
				a->pending_elem[sizeof(a->pending_elem) - 1] = '\0';
				return EVT_CONTENT;
			}
			(*depth)++;
			accum_free_attrs(a);
			accum_content_reset(a);
			strncpy(a->elem_name, x->elem, sizeof(a->elem_name) - 1);
			a->elem_name[sizeof(a->elem_name) - 1] = '\0';
			a->in_attrs = 1;
			break;

		case YXML_ATTRSTART:
			accum_attr_start(a);
			accum_attr_name_append(a, x->attr);
			break;

		case YXML_ATTRVAL:
			accum_attr_value_append(a, x->data);
			break;

		case YXML_ATTREND:
			accum_attr_finish(a);
			break;

		case YXML_CONTENT:
			if (a->in_attrs) {
				/* Attrs done, content starting. Append this first content
				   char and return EVT_ELEM_START. The caller must NOT
				   reset the content accumulator. */
				a->in_attrs = 0;
				accum_content_append(a, x->data);
				return EVT_ELEM_START;
			}
			accum_content_append(a, x->data);
			had_content = 1;
			break;

		case YXML_ELEMEND:
			if (a->in_attrs) {
				/* Self-closing element (<foo/>). Return ELEM_START now,
				   queue ELEM_END for next call. */
				a->in_attrs = 0;
				a->pending_end = 1;
				return EVT_ELEM_START;
			}
			if (had_content) {
				/* Return content first, queue ELEM_END for next call. */
				a->pending_content = 1;
				return EVT_CONTENT;
			}
			(*depth)--;
			return EVT_ELEM_END;

		default:
			break;
		}
	}

	if (a->in_attrs) {
		a->in_attrs = 0;
		return EVT_ELEM_START;
	}
	if (had_content) return EVT_CONTENT;
	return EVT_EOF;
}

/*
	Skip current element and all its children.
	Assumes we just received EVT_ELEM_START for the element to skip.
	curr_depth is the depth *after* the ELEMSTART increment.
*/
static int skip_element(yxml_t* x, const char* buf, size_t len, size_t* pos, xml_accum* a, int* depth, int target_depth) {
	enum xml_event evt;
	while (*depth >= target_depth) {
		evt = next_event(x, buf, len, pos, a, depth);
		if (evt == EVT_ERROR || evt == EVT_EOF) return 0;
	}
	return 1;
}

/*
	Read all text content of the current element until its closing tag.
	Accumulated content is in a->content. Returns 1 on success, 0 on error.
*/
static int read_content(yxml_t* x, const char* buf, size_t len, size_t* pos, xml_accum* a, int* depth, int target_depth) {
	enum xml_event evt;
	/* Do NOT reset content here — the first content char may already
	   have been appended by next_event during the in_attrs→YXML_CONTENT
	   transition. Content was already reset when the element started. */
	while (*depth >= target_depth) {
		evt = next_event(x, buf, len, pos, a, depth);
		if (evt == EVT_CONTENT) continue;
		if (evt == EVT_ELEM_END) {
			if (*depth < target_depth) return 1;
			continue;
		}
		if (evt == EVT_ELEM_START) {
			/* nested element inside content — skip it */
			if (!skip_element(x, buf, len, pos, a, depth, *depth)) return 0;
			continue;
		}
		if (evt == EVT_EOF) return 1;
		return 0; /* error */
	}
	return 1;
}

/*
	 - Element Handlers -
*/

static int parse_property_y(yxml_t* x, const char* buf, size_t len, size_t* pos, xml_accum* a, int* depth, tmx_property* prop) {
	const char* value;
	int my_depth = *depth;

	if ((value = get_attr(a, "name"))) {
		prop->name = tmx_strdup(value);
	}
	else {
		tmx_err(E_MISSEL, "xml parser: missing 'name' attribute in the 'property' element");
		return 0;
	}

	if ((value = get_attr(a, "propertytype"))) {
		prop->propertytype = tmx_strdup(value);
	}

	if ((value = get_attr(a, "type"))) {
		prop->type = parse_property_type(value);
	}
	else {
		prop->type = PT_STRING;
	}

	if ((value = get_attr(a, "value"))) {
		switch (prop->type) {
		case PT_OBJECT:
		case PT_INT:
			prop->value.integer = atoi(value);
			break;
		case PT_FLOAT:
			prop->value.decimal = (float)atof(value);
			break;
		case PT_BOOL:
			prop->value.integer = parse_boolean(value);
			break;
		case PT_COLOR:
			prop->value.integer = get_color_rgb(value);
			break;
		case PT_NONE:
		case PT_STRING:
		case PT_FILE:
		default:
			prop->value.string = tmx_strdup(value);
			break;
		}
	}
	else if (prop->type == PT_NONE || prop->type == PT_STRING) {
		/* Read inner text content */
		if (!read_content(x, buf, len, pos, a, depth, my_depth)) return 0;
		if (a->content && a->content_len > 0) {
			prop->value.string = tmx_strdup(a->content);
		}
		else {
			tmx_err(E_MISSEL, "xml parser: missing 'value' attribute or inner XML for the 'property' element");
			prop->value.string = NULL;
		}
		return 1; /* already consumed closing tag */
	}
	else if (prop->type != PT_CUSTOM) {
		tmx_err(E_MISSEL, "xml parser: missing 'value' attribute in the 'property' element");
		return 0;
	}

	/* Parse children (class-typed properties have nested <properties>) */
	{
		enum xml_event evt;
		char elem_name[256];
		while (*depth >= my_depth) {
			evt = next_event(x, buf, len, pos, a, depth);
			if (evt == EVT_ELEM_END) {
				if (*depth < my_depth) return 1;
			}
			else if (evt == EVT_ELEM_START) {
				strncpy(elem_name, a->elem_name, sizeof(elem_name) - 1);
				elem_name[sizeof(elem_name) - 1] = '\0';
				if (!strcmp(elem_name, "properties")) {
					if (!parse_properties_y(x, buf, len, pos, a, depth, &(prop->value.properties))) return 0;
				}
				else {
					if (!skip_element(x, buf, len, pos, a, depth, *depth)) return 0;
				}
			}
			else if (evt == EVT_CONTENT) {
				continue;
			}
			else if (evt == EVT_EOF) {
				return 1;
			}
			else {
				return 0;
			}
		}
	}

	return 1;
}

static int parse_properties_y(yxml_t* x, const char* buf, size_t len, size_t* pos, xml_accum* a, int* depth, tmx_properties** prop_hashptr) {
	tmx_property* res;
	int my_depth = *depth;
	enum xml_event evt;
	char elem_name[256];

	if (*prop_hashptr == NULL) {
		if (!(*prop_hashptr = (tmx_properties*)mk_hashtable(5))) return 0;
	}

	while (*depth >= my_depth) {
		evt = next_event(x, buf, len, pos, a, depth);
		if (evt == EVT_ELEM_END) {
			if (*depth < my_depth) return 1;
		}
		else if (evt == EVT_ELEM_START) {
			strncpy(elem_name, a->elem_name, sizeof(elem_name) - 1);
			elem_name[sizeof(elem_name) - 1] = '\0';
			if (!strcmp(elem_name, "property")) {
				if (!(res = alloc_prop())) return 0;
				if (!parse_property_y(x, buf, len, pos, a, depth, res)) return 0;
				hashtable_set((void*)*prop_hashptr, res->name, (void*)res, NULL);
			}
			else {
				if (!skip_element(x, buf, len, pos, a, depth, *depth)) return 0;
			}
		}
		else if (evt == EVT_CONTENT) {
			continue;
		}
		else if (evt == EVT_EOF) {
			return 1;
		}
		else {
			return 0;
		}
	}
	return 1;
}

static int parse_points_y(xml_accum* a, tmx_shape* shape) {
	const char* value;
	char* copy, * v;
	int i;

	if (!(value = get_attr(a, "points"))) {
		tmx_err(E_MISSEL, "xml parser: missing 'points' attribute in the 'object' element");
		return 0;
	}

	copy = tmx_strdup(value);

	shape->points_len = 1 + count_char_occurences(copy, ' ');

	shape->points = (double**)tmx_alloc_func(NULL, shape->points_len * sizeof(double*));
	if (!(shape->points)) {
		tmx_free_func(copy);
		tmx_errno = E_ALLOC;
		return 0;
	}

	shape->points[0] = (double*)tmx_alloc_func(NULL, shape->points_len * 2 * sizeof(double));
	if (!(shape->points[0])) {
		tmx_free_func(shape->points);
		tmx_free_func(copy);
		tmx_errno = E_ALLOC;
		return 0;
	}

	for (i = 1; i < shape->points_len; i++) {
		shape->points[i] = shape->points[0] + (i * 2);
	}

	v = copy;
	for (i = 0; i < shape->points_len; i++) {
		if (sscanf(v, "%lf,%lf", shape->points[i], shape->points[i] + 1) != 2) {
			tmx_err(E_XDATA, "xml parser: corrupted point list");
			tmx_free_func(copy);
			return 0;
		}
		v = 1 + strchr(v, ' ');
	}

	tmx_free_func(copy);
	return 1;
}

static int parse_text_y(yxml_t* x, const char* buf, size_t len, size_t* pos, xml_accum* a, int* depth, tmx_text* text) {
	const char* value;
	int my_depth = *depth;

	if ((value = get_attr(a, "fontfamily"))) {
		text->fontfamily = tmx_strdup(value);
	}
	else {
		text->fontfamily = tmx_strdup("sans-serif");
	}

	if ((value = get_attr(a, "pixelsize")))  text->pixelsize = (int)atoi(value);
	if ((value = get_attr(a, "color")))      text->color = get_color_rgb(value);
	if ((value = get_attr(a, "wrap")))        text->wrap = (int)atoi(value);
	if ((value = get_attr(a, "bold")))        text->bold = (int)atoi(value);
	if ((value = get_attr(a, "italic")))      text->italic = (int)atoi(value);
	if ((value = get_attr(a, "underline")))   text->underline = (int)atoi(value);
	if ((value = get_attr(a, "strikeout")))   text->strikeout = (int)atoi(value);
	if ((value = get_attr(a, "kerning")))     text->kerning = (int)atoi(value);
	if ((value = get_attr(a, "halign")))      text->halign = parse_horizontal_align(value);
	if ((value = get_attr(a, "valign")))      text->valign = parse_vertical_align(value);

	/* Read inner text */
	if (!read_content(x, buf, len, pos, a, depth, my_depth)) return 0;
	if (a->content && a->content_len > 0) {
		text->text = tmx_strdup(a->content);
	}

	return 1;
}

static int parse_object_y(yxml_t* x, const char* buf, size_t len, size_t* pos, xml_accum* a, int* depth, tmx_object* obj, int is_on_map, tmx_resource_manager* rc_mgr, const char* filename) {
	int my_depth = *depth;
	const char* value;
	char* ab_path;
	resource_holder* tmpl;
	enum xml_event evt;
	char elem_name[256];

	if ((value = get_attr(a, "id"))) {
		obj->id = atoi(value);
	}
	else if (is_on_map) {
		tmx_err(E_MISSEL, "xml parser: missing 'id' attribute in the 'object' element");
		return 0;
	}

	if ((value = get_attr(a, "x"))) {
		obj->x = atof(value);
	}
	else if (is_on_map) {
		tmx_err(E_MISSEL, "xml parser: missing 'x' attribute in the 'object' element");
		return 0;
	}

	if ((value = get_attr(a, "y"))) {
		obj->y = atof(value);
	}
	else if (is_on_map) {
		tmx_err(E_MISSEL, "xml parser: missing 'y' attribute in the 'object' element");
		return 0;
	}

	if ((value = get_attr(a, "template"))) {
		if (rc_mgr) {
			tmpl = (resource_holder*)hashtable_get((void*)rc_mgr, value);
			if (tmpl && tmpl->type == RC_TX) {
				obj->template_ref = tmpl->resource.template;
			}
		}
		if (!(obj->template_ref)) {
			if (!(ab_path = mk_absolute_path(filename, value))) return 0;
			{
				xml_reader sub_r;
				if (!reader_open_file(&sub_r, ab_path)) {
					tmx_err(E_XDATA, "xml parser: cannot open object template file '%s'", ab_path);
					tmx_free_func(ab_path);
					return 0;
				}
				obj->template_ref = parse_template_document_y(sub_r.buf, sub_r.buf_len, rc_mgr, ab_path);
				reader_close(&sub_r);
			}
			tmx_free_func(ab_path);
			if (!(obj->template_ref)) return 0;
			if (rc_mgr) {
				add_template(rc_mgr, value, obj->template_ref);
			}
			else {
				obj->template_ref->is_embedded = 1;
			}
		}
		obj->obj_type = obj->template_ref->object->obj_type;
	}

	if ((value = get_attr(a, "name")))     obj->name = tmx_strdup(value);
	if ((value = get_attr(a, "type")))     obj->type = tmx_strdup(value);
	else if ((value = get_attr(a, "class"))) obj->type = tmx_strdup(value);
	if ((value = get_attr(a, "visible")))  obj->visible = (char)atoi(value);

	if ((value = get_attr(a, "height"))) {
		obj->obj_type = OT_SQUARE;
		obj->height = atof(value);
	}
	if ((value = get_attr(a, "width")))    obj->width = atof(value);

	if ((value = get_attr(a, "gid"))) {
		unsigned long long gid;
		obj->obj_type = OT_TILE;
		gid = strtoull(value, NULL, 0);
		if (!gid) {
			tmx_err(E_RANGE, "xml parser: object %u has invalid gid '%s'", obj->id, value);
			return 0;
		}
		if (gid > UINT_MAX) {
			tmx_err(E_RANGE, "xml parser: object %u has out-of-range gid '%s'", obj->id, value);
			return 0;
		}
		obj->content.gid = (unsigned int)gid;
	}

	if ((value = get_attr(a, "rotation"))) obj->rotation = atof(value);

	/* Parse children */
	while (*depth >= my_depth) {
		evt = next_event(x, buf, len, pos, a, depth);
		if (evt == EVT_ELEM_END) {
			if (*depth < my_depth) break;
		}
		else if (evt == EVT_ELEM_START) {
			strncpy(elem_name, a->elem_name, sizeof(elem_name) - 1);
			elem_name[sizeof(elem_name) - 1] = '\0';
			if (!strcmp(elem_name, "properties")) {
				if (!parse_properties_y(x, buf, len, pos, a, depth, &(obj->properties))) return 0;
			}
			else if (!strcmp(elem_name, "ellipse")) {
				obj->obj_type = OT_ELLIPSE;
				if (!skip_element(x, buf, len, pos, a, depth, *depth)) return 0;
			}
			else if (!strcmp(elem_name, "point")) {
				/* point type has no children */
				if (!skip_element(x, buf, len, pos, a, depth, *depth)) return 0;
			}
			else if (!strcmp(elem_name, "polygon")) {
				obj->obj_type = OT_POLYGON;
				if (obj->content.shape = alloc_shape(), !(obj->content.shape)) return 0;
				if (!parse_points_y(a, obj->content.shape)) return 0;
				if (!skip_element(x, buf, len, pos, a, depth, *depth)) return 0;
			}
			else if (!strcmp(elem_name, "polyline")) {
				obj->obj_type = OT_POLYLINE;
				if (obj->content.shape = alloc_shape(), !(obj->content.shape)) return 0;
				if (!parse_points_y(a, obj->content.shape)) return 0;
				if (!skip_element(x, buf, len, pos, a, depth, *depth)) return 0;
			}
			else if (!strcmp(elem_name, "text")) {
				obj->obj_type = OT_TEXT;
				if (obj->content.text = alloc_text(), !(obj->content.text)) return 0;
				if (!parse_text_y(x, buf, len, pos, a, depth, obj->content.text)) return 0;
			}
			else {
				if (!skip_element(x, buf, len, pos, a, depth, *depth)) return 0;
			}
		}
		else if (evt == EVT_CONTENT) {
			continue;
		}
		else if (evt == EVT_EOF) {
			break;
		}
		else {
			return 0;
		}
	}

	if (obj->obj_type == OT_NONE) {
		obj->obj_type = OT_POINT;
	}

	return 1;
}

static int parse_data_y(yxml_t* x, const char* buf, size_t len, size_t* pos, xml_accum* a, int* depth, uint32_t** gidsadr, size_t gidscount) {
	const char* value;
	const char* compression;
	char* inner_xml;
	enum enccmp_t data_type;
	int my_depth = *depth;

	if (!(value = get_attr(a, "encoding"))) {
		tmx_err(E_MISSEL, "xml parser: missing 'encoding' attribute in the 'data' element");
		return 0;
	}

	/* Read inner content */
	if (!read_content(x, buf, len, pos, a, depth, my_depth)) return 0;

	if (!a->content || a->content_len == 0) {
		tmx_err(E_XDATA, "xml parser: missing content in the 'data' element");
		return 0;
	}

	inner_xml = tmx_strdup(a->content);

	if (!strcmp(value, "base64")) {
		compression = get_attr(a, "compression");
		data_type = B64;
		if (compression && !strcmp(compression, "zstd")) {
			data_type = B64ZSTD;
		}
		else if (compression && !(strcmp(compression, "zlib") && strcmp(compression, "gzip"))) {
			data_type = B64Z;
		}
		else if (compression) {
			tmx_err(E_ENCCMP, "xml parser: unsupported data compression: '%s'", compression);
			tmx_free_func(inner_xml);
			return 0;
		}
		if (!data_decode(str_trim(inner_xml), data_type, gidscount, gidsadr)) {
			tmx_free_func(inner_xml);
			return 0;
		}
	}
	else if (!strcmp(value, "xml")) {
		tmx_err(E_ENCCMP, "xml parser: unimplemented data encoding: XML");
		tmx_free_func(inner_xml);
		return 0;
	}
	else if (!strcmp(value, "csv")) {
		if (!data_decode(str_trim(inner_xml), CSV, gidscount, gidsadr)) {
			tmx_free_func(inner_xml);
			return 0;
		}
	}
	else {
		tmx_err(E_ENCCMP, "xml parser: unknown data encoding: %s", value);
		tmx_free_func(inner_xml);
		return 0;
	}

	tmx_free_func(inner_xml);
	return 1;
}

static int parse_image_y(xml_accum* a, tmx_image** img_adr, short strict, const char* filename) {
	tmx_image* res;
	const char* value;

	if (!(res = alloc_image())) return 0;
	*img_adr = res;

	if ((value = get_attr(a, "source"))) {
		res->source = tmx_strdup(value);
		if (!(load_image(&(res->resource_image), filename, value))) {
			tmx_err(E_UNKN, "xml parser: an error occured in the delegated image loading function");
			return 0;
		}
	}
	else {
		tmx_err(E_MISSEL, "xml parser: missing 'source' attribute in the 'image' element");
		return 0;
	}

	if ((value = get_attr(a, "height"))) {
		res->height = atoi(value);
	}
	else if (strict) {
		tmx_err(E_MISSEL, "xml parser: missing 'height' attribute in the 'image' element");
		return 0;
	}

	if ((value = get_attr(a, "width"))) {
		res->width = atoi(value);
	}
	else if (strict) {
		tmx_err(E_MISSEL, "xml parser: missing 'width' attribute in the 'image' element");
		return 0;
	}

	if ((value = get_attr(a, "trans"))) {
		res->trans = get_color_rgb(value);
		res->uses_trans = 1;
	}

	return 1;
}

static int parse_tileoffset_y(xml_accum* a, int* ox, int* oy) {
	const char* value;
	if ((value = get_attr(a, "x"))) {
		*ox = atoi(value);
	}
	else {
		tmx_err(E_MISSEL, "xml parser: missing 'x' attribute in the 'tileoffset' element");
		return 0;
	}
	if ((value = get_attr(a, "y"))) {
		*oy = atoi(value);
	}
	else {
		tmx_err(E_MISSEL, "xml parser: missing 'y' attribute in the 'tileoffset' element");
		return 0;
	}
	return 1;
}

/*
	Animation parser — collects frames into a linked list on the stack
	then copies to heap, matching the original recursive approach.
*/
static int parse_animation_y(yxml_t* x, const char* buf, size_t len, size_t* pos, xml_accum* a, int* depth, tmx_tile* tile) {
	int my_depth = *depth;
	enum xml_event evt;
	char elem_name[256];
	const char* value;

	/* Two-pass: first count frames, then parse them */
	/* Actually, we'll collect them in a temporary array */
	int cap = 16, count = 0;
	tmx_anim_frame* frames = (tmx_anim_frame*)tmx_alloc_func(NULL, cap * sizeof(tmx_anim_frame));
	if (!frames) { tmx_errno = E_ALLOC; return 0; }

	while (*depth >= my_depth) {
		evt = next_event(x, buf, len, pos, a, depth);
		if (evt == EVT_ELEM_END) {
			if (*depth < my_depth) break;
		}
		else if (evt == EVT_ELEM_START) {
			strncpy(elem_name, a->elem_name, sizeof(elem_name) - 1);
			elem_name[sizeof(elem_name) - 1] = '\0';
			if (!strcmp(elem_name, "frame")) {
				if (count >= cap) {
					cap *= 2;
					frames = (tmx_anim_frame*)tmx_alloc_func(frames, cap * sizeof(tmx_anim_frame));
					if (!frames) { tmx_errno = E_ALLOC; return 0; }
				}
				if ((value = get_attr(a, "tileid"))) {
					frames[count].tile_id = atoi(value);
				}
				else {
					tmx_err(E_MISSEL, "xml parser: missing 'tileid' attribute in the 'frame' element");
					tmx_free_func(frames);
					return 0;
				}
				if ((value = get_attr(a, "duration"))) {
					frames[count].duration = atoi(value);
				}
				else {
					tmx_err(E_MISSEL, "xml parser: missing 'duration' attribute in the 'frame' element");
					tmx_free_func(frames);
					return 0;
				}
				count++;
				if (!skip_element(x, buf, len, pos, a, depth, *depth)) {
					tmx_free_func(frames);
					return 0;
				}
			}
			else {
				if (!skip_element(x, buf, len, pos, a, depth, *depth)) {
					tmx_free_func(frames);
					return 0;
				}
			}
		}
		else if (evt == EVT_CONTENT) {
			continue;
		}
		else if (evt == EVT_EOF) {
			break;
		}
		else {
			tmx_free_func(frames);
			return 0;
		}
	}

	if (count > 0) {
		/* Shrink to fit */
		tile->animation = (tmx_anim_frame*)tmx_alloc_func(NULL, count * sizeof(tmx_anim_frame));
		if (!tile->animation) {
			tmx_free_func(frames);
			tmx_errno = E_ALLOC;
			return 0;
		}
		memcpy(tile->animation, frames, count * sizeof(tmx_anim_frame));
		tile->animation_len = count;
	}
	tmx_free_func(frames);

	return 1;
}

static int parse_tile_y(yxml_t* x, const char* buf, size_t len, size_t* pos, xml_accum* a, int* depth, tmx_tileset* tileset, tmx_resource_manager* rc_mgr, const char* filename) {
	tmx_tile* res = NULL;
	tmx_object* obj;
	unsigned int id;
	int my_depth = *depth;
	int tlen, to_move;
	int has_width = 0, has_height = 0;
	const char* value;
	enum xml_event evt;
	char elem_name[256];

	if ((value = get_attr(a, "id"))) {
		id = atoi(value);
		/* Insertion sort */
		tlen = tileset->user_data.integer;
		for (to_move = 0; (tlen - 1) - to_move >= 0; to_move++) {
			if (tileset->tiles[(tlen - 1) - to_move].id < id) {
				break;
			}
		}
		if (to_move > 0) {
			memmove((tileset->tiles) + (tlen - to_move + 1), (tileset->tiles) + (tlen - to_move), to_move * sizeof(tmx_tile));
		}
		res = &(tileset->tiles[tlen - to_move]);

		if ((unsigned int)(tileset->user_data.integer) == tileset->tilecount) {
			tileset->user_data.integer = 0;
		}
		else {
			tileset->user_data.integer += 1;
		}
		res->id = id;
		res->tileset = tileset;
	}
	else {
		tmx_err(E_MISSEL, "xml parser: missing 'id' attribute in the 'tile' element");
		return 0;
	}

	res->ul_x = res->ul_y = 0;
	res->width = res->height = -1;

	if ((value = get_attr(a, "type")))       res->type = tmx_strdup(value);
	else if ((value = get_attr(a, "class"))) res->type = tmx_strdup(value);

	if ((value = get_attr(a, "x")))      res->ul_x = atoi(value);
	if ((value = get_attr(a, "y")))      res->ul_y = atoi(value);
	if ((value = get_attr(a, "width"))) { res->width = atoi(value); has_width = 1; }
	if ((value = get_attr(a, "height"))) { res->height = atoi(value); has_height = 1; }

	/* Parse children */
	while (*depth >= my_depth) {
		evt = next_event(x, buf, len, pos, a, depth);
		if (evt == EVT_ELEM_END) {
			if (*depth < my_depth) break;
		}
		else if (evt == EVT_ELEM_START) {
			strncpy(elem_name, a->elem_name, sizeof(elem_name) - 1);
			elem_name[sizeof(elem_name) - 1] = '\0';
			if (!strcmp(elem_name, "properties")) {
				if (!parse_properties_y(x, buf, len, pos, a, depth, &(res->properties))) return 0;
			}
			else if (!strcmp(elem_name, "image")) {
				if (!parse_image_y(a, &(res->image), 0, filename)) return 0;
				if (!skip_element(x, buf, len, pos, a, depth, *depth)) return 0;
			}
			else if (!strcmp(elem_name, "objectgroup")) {
				/* tile collision */
				int objgr_depth = *depth;
				while (*depth >= objgr_depth) {
					evt = next_event(x, buf, len, pos, a, depth);
					if (evt == EVT_ELEM_END) {
						if (*depth < objgr_depth) break;
					}
					else if (evt == EVT_ELEM_START) {
						strncpy(elem_name, a->elem_name, sizeof(elem_name) - 1);
						elem_name[sizeof(elem_name) - 1] = '\0';
						if (!strcmp(elem_name, "object")) {
							if (!(obj = alloc_object())) return 0;
							obj->next = res->collision;
							res->collision = obj;
							if (!parse_object_y(x, buf, len, pos, a, depth, obj, 0, rc_mgr, filename)) return 0;
						}
						else {
							if (!skip_element(x, buf, len, pos, a, depth, *depth)) return 0;
						}
					}
					else if (evt == EVT_CONTENT) {
						continue;
					}
					else if (evt == EVT_EOF) {
						break;
					}
					else {
						return 0;
					}
				}
			}
			else if (!strcmp(elem_name, "animation")) {
				if (!parse_animation_y(x, buf, len, pos, a, depth, res)) return 0;
			}
			else {
				if (!skip_element(x, buf, len, pos, a, depth, *depth)) return 0;
			}
		}
		else if (evt == EVT_CONTENT) {
			continue;
		}
		else if (evt == EVT_EOF) {
			break;
		}
		else {
			return 0;
		}
	}

	if (res->image) {
		if (!has_width) res->width = res->image->width;
		if (!has_height) res->height = res->image->height;
	}

	return 1;
}

static int parse_tileset_y(yxml_t* x, const char* buf, size_t len, size_t* pos, xml_accum* a, int* depth, tmx_tileset* ts, tmx_resource_manager* rc_mgr, const char* filename) {
	int my_depth = *depth;
	const char* value;
	enum xml_event evt;
	char elem_name[256];

	if ((value = get_attr(a, "name"))) {
		ts->name = tmx_strdup(value);
	}
	else {
		tmx_err(E_MISSEL, "xml parser: missing 'name' attribute in the 'tileset' element");
		return 0;
	}

	if ((value = get_attr(a, "class")))           ts->class_type = tmx_strdup(value);

	if ((value = get_attr(a, "tilecount"))) {
		ts->tilecount = atoi(value);
	}
	else {
		tmx_err(E_MISSEL, "xml parser: missing 'tilecount' attribute in the 'tileset' element");
		return 0;
	}

	if ((value = get_attr(a, "tilewidth"))) {
		ts->tile_width = atoi(value);
	}
	else {
		tmx_err(E_MISSEL, "xml parser: missing 'tilewidth' attribute in the 'tileset' element");
		return 0;
	}

	if ((value = get_attr(a, "tileheight"))) {
		ts->tile_height = atoi(value);
	}
	else {
		tmx_err(E_MISSEL, "xml parser: missing 'tileheight' attribute in the 'tileset' element");
		return 0;
	}

	if ((value = get_attr(a, "spacing")))          ts->spacing = atoi(value);
	if ((value = get_attr(a, "margin")))           ts->margin = atoi(value);
	if ((value = get_attr(a, "objectalignment")))  ts->objectalignment = parse_obj_alignment(value);
	if ((value = get_attr(a, "tilerendersize")))   ts->tile_render_size = parse_tile_render_size(value);
	if ((value = get_attr(a, "fillmode")))         ts->fill_mode = parse_fillmode(value);

	if (!(ts->tiles = alloc_tiles(ts->tilecount))) return 0;

	/* Parse children */
	while (*depth >= my_depth) {
		evt = next_event(x, buf, len, pos, a, depth);
		if (evt == EVT_ELEM_END) {
			if (*depth < my_depth) break;
		}
		else if (evt == EVT_ELEM_START) {
			strncpy(elem_name, a->elem_name, sizeof(elem_name) - 1);
			elem_name[sizeof(elem_name) - 1] = '\0';
			if (!strcmp(elem_name, "image")) {
				if (!parse_image_y(a, &(ts->image), 1, filename)) return 0;
				if (!skip_element(x, buf, len, pos, a, depth, *depth)) return 0;
			}
			else if (!strcmp(elem_name, "tileoffset")) {
				if (!parse_tileoffset_y(a, &(ts->x_offset), &(ts->y_offset))) return 0;
				if (!skip_element(x, buf, len, pos, a, depth, *depth)) return 0;
			}
			else if (!strcmp(elem_name, "properties")) {
				if (!parse_properties_y(x, buf, len, pos, a, depth, &(ts->properties))) return 0;
			}
			else if (!strcmp(elem_name, "tile")) {
				if (!parse_tile_y(x, buf, len, pos, a, depth, ts, rc_mgr, filename)) return 0;
			}
			else {
				if (!skip_element(x, buf, len, pos, a, depth, *depth)) return 0;
			}
		}
		else if (evt == EVT_CONTENT) {
			continue;
		}
		else if (evt == EVT_EOF) {
			break;
		}
		else {
			return 0;
		}
	}

	if (ts->image && !set_tiles_runtime_props(ts)) return 0;

	return 1;
}

static int parse_tileset_list_y(yxml_t* x, const char* buf, size_t len, size_t* pos, xml_accum* a, int* depth, tmx_tileset_list** ts_headadr, tmx_resource_manager* rc_mgr, const char* filename) {
	tmx_tileset_list* res_list = NULL;
	tmx_tileset* res = NULL;
	resource_holder* rc_holder;
	const char* value;
	char* ab_path;

	if (!(res_list = alloc_tileset_list())) return 0;
	res_list->next = *ts_headadr;
	*ts_headadr = res_list;

	if ((value = get_attr(a, "firstgid"))) {
		res_list->firstgid = atoi(value);
	}
	else {
		tmx_err(E_MISSEL, "xml parser: missing 'firstgid' attribute in the 'tileset' element");
		return 0;
	}

	/* External tileset */
	if ((value = get_attr(a, "source"))) {
		res_list->source = tmx_strdup(value);
		if (rc_mgr) {
			rc_holder = (resource_holder*)hashtable_get((void*)rc_mgr, value);
			if (rc_holder && rc_holder->type == RC_TSX) {
				res = rc_holder->resource.tileset;
				if (res) {
					res_list->tileset = res;
					if (!skip_element(x, buf, len, pos, a, depth, *depth)) return 0;
					return 1;
				}
			}
		}
		if (!(res = alloc_tileset())) return 0;
		res_list->tileset = res;
		if (rc_mgr) {
			add_tileset(rc_mgr, value, res);
		}
		else {
			res_list->is_embedded = 1;
		}
		if (!(ab_path = mk_absolute_path(filename, value))) return 0;
		{
			xml_reader sub_r;
			yxml_t sub_x;
			char sub_stack[8192];
			xml_accum sub_a;
			int sub_depth = 0;
			size_t sub_pos = 0;
			enum xml_event sub_evt;
			int ret = 0;

			if (!reader_open_file(&sub_r, ab_path)) {
				tmx_err(E_XDATA, "xml parser: cannot open extern tileset '%s'", ab_path);
				tmx_free_func(ab_path);
				return 0;
			}

			yxml_init(&sub_x, sub_stack, sizeof(sub_stack));
			accum_init(&sub_a);

			/* Find root <tileset> element */
			sub_evt = next_event(&sub_x, sub_r.buf, sub_r.buf_len, &sub_pos, &sub_a, &sub_depth);
			if (sub_evt == EVT_ELEM_START && !strcmp(sub_a.elem_name, "tileset")) {
				ret = parse_tileset_y(&sub_x, sub_r.buf, sub_r.buf_len, &sub_pos, &sub_a, &sub_depth, res, rc_mgr, ab_path);
			}
			else {
				tmx_err(E_XDATA, "xml parser: root of tileset document is not a 'tileset' element");
			}

			accum_destroy(&sub_a);
			reader_close(&sub_r);
			tmx_free_func(ab_path);

			if (!ret) return 0;
		}
		if (!skip_element(x, buf, len, pos, a, depth, *depth)) return 0;
		return 1;
	}

	/* Embedded tileset */
	if (!(res = alloc_tileset())) return 0;
	res_list->is_embedded = 1;
	res_list->tileset = res;

	return parse_tileset_y(x, buf, len, pos, a, depth, res, rc_mgr, filename);
}

static int parse_layer_y(yxml_t* x, const char* buf, size_t len, size_t* pos, xml_accum* a, int* depth, tmx_layer** layer_headadr, int map_h, int map_w, enum tmx_layer_type type, tmx_resource_manager* rc_mgr, const char* filename) {
	tmx_layer* res;
	tmx_object* obj;
	int my_depth = *depth;
	const char* value;
	enum xml_event evt;
	char elem_name[256];
	enum tmx_layer_type child_type;

	if (!(res = alloc_layer())) return 0;
	res->type = type;
	while (*layer_headadr) {
		layer_headadr = &((*layer_headadr)->next);
	}
	*layer_headadr = res;

	/* Read attributes */
	if ((value = get_attr(a, "id")))         res->id = atoi(value);
	if ((value = get_attr(a, "name"))) {
		res->name = tmx_strdup(value);
	}
	else {
		tmx_err(E_MISSEL, "xml parser: missing 'name' attribute in the 'layer' element");
		return 0;
	}
	if ((value = get_attr(a, "class")))      res->class_type = tmx_strdup(value);
	if ((value = get_attr(a, "visible")))    res->visible = (char)atoi(value);
	if ((value = get_attr(a, "opacity")))    res->opacity = atof(value);
	if ((value = get_attr(a, "offsetx")))    res->offsetx = (int)atoi(value);
	if ((value = get_attr(a, "offsety")))    res->offsety = (int)atoi(value);
	if ((value = get_attr(a, "parallaxx")))  res->parallaxx = atof(value);
	if ((value = get_attr(a, "parallaxy")))  res->parallaxy = atof(value);
	if ((value = get_attr(a, "tintcolor")))  res->tintcolor = get_color_rgb(value);

	if (type == L_OBJGR) {
		tmx_object_group* objgr = alloc_objgr();
		res->content.objgr = objgr;
		if ((value = get_attr(a, "color")))      objgr->color = get_color_rgb(value);
		value = get_attr(a, "draworder");
		objgr->draworder = parse_objgr_draworder(value);
	}

	if (type == L_IMAGE) {
		if ((value = get_attr(a, "repeatx")))  res->repeatx = atoi(value);
		if ((value = get_attr(a, "repeaty")))  res->repeaty = atoi(value);
	}

	/* Parse children */
	while (*depth >= my_depth) {
		evt = next_event(x, buf, len, pos, a, depth);
		if (evt == EVT_ELEM_END) {
			if (*depth < my_depth) break;
		}
		else if (evt == EVT_ELEM_START) {
			strncpy(elem_name, a->elem_name, sizeof(elem_name) - 1);
			elem_name[sizeof(elem_name) - 1] = '\0';
			if (!strcmp(elem_name, "properties")) {
				if (!parse_properties_y(x, buf, len, pos, a, depth, &(res->properties))) return 0;
			}
			else if (!strcmp(elem_name, "data")) {
				if (!parse_data_y(x, buf, len, pos, a, depth, &(res->content.gids), map_h * map_w)) return 0;
			}
			else if (!strcmp(elem_name, "image")) {
				if (!parse_image_y(a, &(res->content.image), 0, filename)) return 0;
				if (!skip_element(x, buf, len, pos, a, depth, *depth)) return 0;
			}
			else if (!strcmp(elem_name, "object")) {
				if (!(obj = alloc_object())) return 0;
				obj->next = res->content.objgr->head;
				res->content.objgr->head = obj;
				if (!parse_object_y(x, buf, len, pos, a, depth, obj, 1, rc_mgr, filename)) return 0;
			}
			else if (type == L_GROUP && (child_type = parse_layer_type(elem_name)) != L_NONE) {
				if (!parse_layer_y(x, buf, len, pos, a, depth, &(res->content.group_head), map_h, map_w, child_type, rc_mgr, filename)) return 0;
			}
			else {
				if (!skip_element(x, buf, len, pos, a, depth, *depth)) return 0;
			}
		}
		else if (evt == EVT_CONTENT) {
			continue;
		}
		else if (evt == EVT_EOF) {
			break;
		}
		else {
			return 0;
		}
	}

	return 1;
}

static int parse_template_y(yxml_t* x, const char* buf, size_t len, size_t* pos, xml_accum* a, int* depth, tmx_template* tmpl, tmx_resource_manager* rc_mgr, const char* filename) {
	int my_depth = *depth;
	enum xml_event evt;
	char elem_name[256];

	while (*depth >= my_depth) {
		evt = next_event(x, buf, len, pos, a, depth);
		if (evt == EVT_ELEM_END) {
			if (*depth < my_depth) break;
		}
		else if (evt == EVT_ELEM_START) {
			strncpy(elem_name, a->elem_name, sizeof(elem_name) - 1);
			elem_name[sizeof(elem_name) - 1] = '\0';
			if (!strcmp(elem_name, "tileset")) {
				if (!parse_tileset_list_y(x, buf, len, pos, a, depth, &(tmpl->tileset_ref), rc_mgr, filename)) return 0;
			}
			else if (!strcmp(elem_name, "object")) {
				if (!parse_object_y(x, buf, len, pos, a, depth, tmpl->object, 0, rc_mgr, filename)) return 0;
			}
			else {
				if (!skip_element(x, buf, len, pos, a, depth, *depth)) return 0;
			}
		}
		else if (evt == EVT_CONTENT) {
			continue;
		}
		else if (evt == EVT_EOF) {
			break;
		}
		else {
			return 0;
		}
	}
	return 1;
}

static int parse_map_y(yxml_t* x, const char* buf, size_t len, size_t* pos, xml_accum* a, int* depth, tmx_map* map, tmx_resource_manager* rc_mgr, const char* filename) {
	int my_depth = *depth;
	int flag;
	const char* value;
	enum xml_event evt;
	char elem_name[256];
	enum tmx_layer_type type;

	if ((value = get_attr(a, "version")))  map->format_version = tmx_strdup(value);
	if ((value = get_attr(a, "class")))    map->class_type = tmx_strdup(value);

	if ((value = get_attr(a, "infinite"))) {
		flag = atoi(value);
		if (flag == 1) {
			tmx_err(E_XDATA, "xml parser: chunked layer data is not supported, edit this map to remove the infinite flag");
			return 0;
		}
	}

	if ((value = get_attr(a, "orientation"))) {
		if (map->orient = parse_orient(value), map->orient == O_NONE) {
			tmx_err(E_XDATA, "xml parser: unsupported 'orientation' '%s'", value);
			return 0;
		}
	}
	else {
		tmx_err(E_MISSEL, "xml parser: missing 'orientation' attribute in the 'map' element");
		return 0;
	}

	value = get_attr(a, "staggerindex");
	if (value != NULL && (map->stagger_index = parse_stagger_index(value), map->stagger_index == SI_NONE)) {
		tmx_err(E_XDATA, "xml parser: unsupported 'staggerindex' '%s'", value);
		return 0;
	}

	value = get_attr(a, "staggeraxis");
	if (map->stagger_axis = parse_stagger_axis(value), map->stagger_axis == SA_NONE) {
		tmx_err(E_XDATA, "xml parser: unsupported 'staggeraxis' '%s'", value);
		return 0;
	}

	value = get_attr(a, "renderorder");
	if (map->renderorder = parse_renderorder(value), map->renderorder == R_NONE) {
		tmx_err(E_XDATA, "xml parser: unsupported 'renderorder' '%s'", value);
		return 0;
	}

	if ((value = get_attr(a, "height"))) {
		map->height = atoi(value);
	}
	else {
		tmx_err(E_MISSEL, "xml parser: missing 'height' attribute in the 'map' element");
		return 0;
	}

	if ((value = get_attr(a, "width"))) {
		map->width = atoi(value);
	}
	else {
		tmx_err(E_MISSEL, "xml parser: missing 'width' attribute in the 'map' element");
		return 0;
	}

	if ((value = get_attr(a, "tileheight"))) {
		map->tile_height = atoi(value);
	}
	else {
		tmx_err(E_MISSEL, "xml parser: missing 'tileheight' attribute in the 'map' element");
		return 0;
	}

	if ((value = get_attr(a, "tilewidth"))) {
		map->tile_width = atoi(value);
	}
	else {
		tmx_err(E_MISSEL, "xml parser: missing 'tilewidth' attribute in the 'map' element");
		return 0;
	}

	if ((value = get_attr(a, "backgroundcolor")))  map->backgroundcolor = get_color_rgb(value);
	if ((value = get_attr(a, "hexsidelength")))    map->hexsidelength = atoi(value);
	if ((value = get_attr(a, "parallaxoriginx")))  map->parallaxoriginx = atof(value);
	if ((value = get_attr(a, "parallaxoriginy")))  map->parallaxoriginy = atof(value);

	/* Parse children */
	while (*depth >= my_depth) {
		evt = next_event(x, buf, len, pos, a, depth);
		if (evt == EVT_ELEM_END) {
			if (*depth < my_depth) break;
		}
		else if (evt == EVT_ELEM_START) {
			strncpy(elem_name, a->elem_name, sizeof(elem_name) - 1);
			elem_name[sizeof(elem_name) - 1] = '\0';
			if (!strcmp(elem_name, "tileset")) {
				if (!parse_tileset_list_y(x, buf, len, pos, a, depth, &(map->ts_head), rc_mgr, filename)) return 0;
			}
			else if (!strcmp(elem_name, "properties")) {
				if (!parse_properties_y(x, buf, len, pos, a, depth, &(map->properties))) return 0;
			}
			else if ((type = parse_layer_type(elem_name)) != L_NONE) {
				if (!parse_layer_y(x, buf, len, pos, a, depth, &(map->ly_head), map->height, map->width, type, rc_mgr, filename)) return 0;
			}
			else {
				if (!skip_element(x, buf, len, pos, a, depth, *depth)) return 0;
			}
		}
		else if (evt == EVT_CONTENT) {
			continue;
		}
		else if (evt == EVT_EOF) {
			break;
		}
		else {
			return 0;
		}
	}
	return 1;
}

/*
	Document-level parsers
*/

static tmx_map* parse_map_document_y(const char* buf, size_t len, tmx_resource_manager* rc_mgr, const char* filename) {
	yxml_t x;
	char stack[8192];
	xml_accum a;
	int depth = 0;
	size_t pos = 0;
	enum xml_event evt;
	tmx_map* res = NULL;

	yxml_init(&x, stack, sizeof(stack));
	accum_init(&a);

	/* Skip to root element (may have <?xml?> PI and/or <!DOCTYPE>) */
	evt = next_event(&x, buf, len, &pos, &a, &depth);
	while (evt == EVT_CONTENT || evt == EVT_ELEM_END) {
		evt = next_event(&x, buf, len, &pos, &a, &depth);
	}

	if (evt != EVT_ELEM_START) {
		tmx_err(E_XDATA, "xml parser: failed to parse document");
		accum_destroy(&a);
		return NULL;
	}

	if (strcmp(a.elem_name, "map")) {
		tmx_err(E_XDATA, "xml parser: root of map document is not a 'map' element");
		accum_destroy(&a);
		return NULL;
	}

	if ((res = alloc_map())) {
		if (!parse_map_y(&x, buf, len, &pos, &a, &depth, res, rc_mgr, filename)) {
			tmx_map_free(res);
			res = NULL;
		}
	}

	accum_destroy(&a);
	return res;
}

static tmx_tileset* parse_tileset_document_y(const char* buf, size_t len, const char* filename) {
	yxml_t x;
	char stack[8192];
	xml_accum a;
	int depth = 0;
	size_t pos = 0;
	enum xml_event evt;
	tmx_tileset* res = NULL;

	yxml_init(&x, stack, sizeof(stack));
	accum_init(&a);

	evt = next_event(&x, buf, len, &pos, &a, &depth);
	while (evt == EVT_CONTENT || evt == EVT_ELEM_END) {
		evt = next_event(&x, buf, len, &pos, &a, &depth);
	}

	if (evt != EVT_ELEM_START) {
		tmx_err(E_XDATA, "xml parser: failed to parse document");
		accum_destroy(&a);
		return NULL;
	}

	if (strcmp(a.elem_name, "tileset")) {
		tmx_err(E_XDATA, "xml parser: root of tileset document is not a 'tileset' element");
		accum_destroy(&a);
		return NULL;
	}

	if ((res = alloc_tileset())) {
		if (!parse_tileset_y(&x, buf, len, &pos, &a, &depth, res, NULL, filename)) {
			free_ts(res);
			res = NULL;
		}
	}

	accum_destroy(&a);
	return res;
}

static tmx_template* parse_template_document_y(const char* buf, size_t len, tmx_resource_manager* rc_mgr, const char* filename) {
	yxml_t x;
	char stack[8192];
	xml_accum a;
	int depth = 0;
	size_t pos = 0;
	enum xml_event evt;
	tmx_template* res = NULL;

	yxml_init(&x, stack, sizeof(stack));
	accum_init(&a);

	evt = next_event(&x, buf, len, &pos, &a, &depth);
	while (evt == EVT_CONTENT || evt == EVT_ELEM_END) {
		evt = next_event(&x, buf, len, &pos, &a, &depth);
	}

	if (evt != EVT_ELEM_START) {
		tmx_err(E_XDATA, "xml parser: failed to parse document");
		accum_destroy(&a);
		return NULL;
	}

	if (strcmp(a.elem_name, "template")) {
		tmx_err(E_XDATA, "xml parser: root of template document is not a 'template' element");
		accum_destroy(&a);
		return NULL;
	}

	if ((res = alloc_template())) {
		if (!parse_template_y(&x, buf, len, &pos, &a, &depth, res, rc_mgr, filename)) {
			free_template(res);
			res = NULL;
		}
	}

	accum_destroy(&a);
	return res;
}

/*
	Public TMX load functions
*/

tmx_map* parse_xml(tmx_resource_manager* rc_mgr, const char* filename) {
	xml_reader r;
	tmx_map* res;
	if (!reader_open_file(&r, filename)) {
		tmx_err(E_UNKN, "xml parser: unable to open %s", filename);
		return NULL;
	}
	res = parse_map_document_y(r.buf, r.buf_len, rc_mgr, filename);
	reader_close(&r);
	return res;
}

tmx_map* parse_xml_buffer(tmx_resource_manager* rc_mgr, const char* buffer, int len) {
	return parse_xml_buffer_vpath(rc_mgr, buffer, len, NULL);
}

tmx_map* parse_xml_buffer_vpath(tmx_resource_manager* rc_mgr, const char* buffer, int len, const char* vpath) {
	return parse_map_document_y(buffer, (size_t)len, rc_mgr, vpath);
}

tmx_map* parse_xml_fd(tmx_resource_manager* rc_mgr, int fd) {
	return parse_xml_fd_vpath(rc_mgr, fd, NULL);
}

tmx_map* parse_xml_fd_vpath(tmx_resource_manager* rc_mgr, int fd, const char* vpath) {
	xml_reader r;
	tmx_map* res;
	if (!reader_open_fd(&r, fd)) {
		tmx_err(E_UNKN, "xml parser: unable create parser for file descriptor");
		return NULL;
	}
	res = parse_map_document_y(r.buf, r.buf_len, rc_mgr, vpath);
	reader_close(&r);
	return res;
}

tmx_map* parse_xml_callback(tmx_resource_manager* rc_mgr, tmx_read_functor callback, void* userdata) {
	return parse_xml_callback_vpath(rc_mgr, callback, NULL, userdata);
}

tmx_map* parse_xml_callback_vpath(tmx_resource_manager* rc_mgr, tmx_read_functor callback, const char* vpath, void* userdata) {
	xml_reader r;
	tmx_map* res;
	if (!reader_open_callback(&r, callback, userdata)) {
		tmx_err(E_UNKN, "xml parser: unable to create parser for input callback");
		return NULL;
	}
	res = parse_map_document_y(r.buf, r.buf_len, rc_mgr, vpath);
	reader_close(&r);
	return res;
}

/*
	Public TSX load functions
*/

tmx_tileset* parse_tsx_xml(const char* filename) {
	xml_reader r;
	tmx_tileset* res;
	if (!reader_open_file(&r, filename)) {
		tmx_err(E_UNKN, "xml parser: unable to open %s", filename);
		return NULL;
	}
	res = parse_tileset_document_y(r.buf, r.buf_len, filename);
	reader_close(&r);
	return res;
}

tmx_tileset* parse_tsx_xml_buffer(const char* buffer, int len) {
	return parse_tileset_document_y(buffer, (size_t)len, NULL);
}

tmx_tileset* parse_tsx_xml_fd(int fd) {
	xml_reader r;
	tmx_tileset* res;
	if (!reader_open_fd(&r, fd)) {
		tmx_err(E_UNKN, "xml parser: unable create parser for file descriptor");
		return NULL;
	}
	res = parse_tileset_document_y(r.buf, r.buf_len, NULL);
	reader_close(&r);
	return res;
}

tmx_tileset* parse_tsx_xml_callback(tmx_read_functor callback, void* userdata) {
	xml_reader r;
	tmx_tileset* res;
	if (!reader_open_callback(&r, callback, userdata)) {
		tmx_err(E_UNKN, "xml parser: unable to create parser for input callback");
		return NULL;
	}
	res = parse_tileset_document_y(r.buf, r.buf_len, NULL);
	reader_close(&r);
	return res;
}

/*
	Public TX load functions
*/

tmx_template* parse_tx_xml(tmx_resource_manager* rc_mgr, const char* filename) {
	xml_reader r;
	tmx_template* res;
	if (!reader_open_file(&r, filename)) {
		tmx_err(E_UNKN, "xml parser: unable to open %s", filename);
		return NULL;
	}
	res = parse_template_document_y(r.buf, r.buf_len, rc_mgr, filename);
	reader_close(&r);
	return res;
}

tmx_template* parse_tx_xml_buffer(tmx_resource_manager* rc_mgr, const char* buffer, int len) {
	return parse_template_document_y(buffer, (size_t)len, rc_mgr, NULL);
}

tmx_template* parse_tx_xml_fd(tmx_resource_manager* rc_mgr, int fd) {
	xml_reader r;
	tmx_template* res;
	if (!reader_open_fd(&r, fd)) {
		tmx_err(E_UNKN, "xml parser: unable create parser for file descriptor");
		return NULL;
	}
	res = parse_template_document_y(r.buf, r.buf_len, rc_mgr, NULL);
	reader_close(&r);
	return res;
}

tmx_template* parse_tx_xml_callback(tmx_resource_manager* rc_mgr, tmx_read_functor callback, void* userdata) {
	xml_reader r;
	tmx_template* res;
	if (!reader_open_callback(&r, callback, userdata)) {
		tmx_err(E_UNKN, "xml parser: unable to create parser for input callback");
		return NULL;
	}
	res = parse_template_document_y(r.buf, r.buf_len, rc_mgr, NULL);
	reader_close(&r);
	return res;
}
