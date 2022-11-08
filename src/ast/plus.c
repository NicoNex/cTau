#include "ast.h"

struct plus {
	struct node *l;
	struct node *r;
};

int compile_plus(struct node *n, struct compiler *c) {
	return -1;
}

void dispose_plus(struct node *n) {
	puts("freeing plus");
	struct plus *p = (struct plus *) n->data;
	if (p->l != NULL) p->l->dispose(p->l);
	if (p->r != NULL) p->r->dispose(p->r);

	free(p);
	free(n);
}

struct node *new_plus(struct node *l, struct node *r) {
	struct plus *p = calloc(1, sizeof(struct plus));
	p->l = l;
	p->r = r;

	return new_node(p, PlusNode, compile_plus, dispose_plus);
}
