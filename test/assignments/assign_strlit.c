char *c = "A42";   {{A}}
char *c = "A4,54"; {{B}}

/*
 * check-name: string literal
 * assert-obj: A != B
 */
