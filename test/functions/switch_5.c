void a(){
	int b;
	switch(b){
		case 42: break;
		default: {{A: break;}} {{B: ;}}
	}
}

/*
 * check-name: default without break
 * A != B
 */
