int i;
typeof (i) x; {{A}}
int x;        {{B}}

/*
 * check-name: typeof (int vs typeof(i); i is int var)
 * obj-not-diff: yes
 * assert-ast: A != B
 * assert-obj: A == B
 */
