int a = 1; {{A}}
int a = 1.444; {{B}}

/*
 * check-name: implicit cast
 * obj-not-diff: maybe
 * B != A
 */
