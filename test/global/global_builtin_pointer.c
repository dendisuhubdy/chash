int *first; {{A}}
int *first; {{C}}
unsigned int *first; {{B}}

{{A:char *}}{{B:long *}}{{C:char *}} second;

typedef int unused; {{C}}
/*
 * check-name: Definition of Global Variable with Pointer 2
 * obj-not-diff: yes
 * A == C, B != A
 */
