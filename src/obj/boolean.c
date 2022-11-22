#include <stdlib.h>
#include "obj.h"

static void dispose_boolean(struct object *o) {
	free(o);
}

struct object *new_boolean_obj(int b) {
	struct object *o = calloc(1, sizeof(struct object));
	o->data.i = b != 0;
	o->type = obj_boolean;
	o->dispose = dispose_boolean;

	return o;
}

