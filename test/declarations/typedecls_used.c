struct s { {{A}}
  int a;   {{A}}
  int b;   {{A}}
};         {{A}}

enum e {ea, eb}; {{A}}

typedef struct s s_type; {{A}}

s_type s1 = {.a = 0, .b = 1}; {{A}}

{{B}}

/*
 * check-name: used Decls
 * assert-obj: A != B
 */
