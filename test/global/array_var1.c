int s = 25;

int func(){
	int a[5]; {{A}}
	int a[s/5]; {{B}}
}
/*
 * check-name: Array var
 * obj-not-diff: jupp
 * B != A
 */
