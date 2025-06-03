#ifndef RAMFORGE_USER_H
#define RAMFORGE_USER_H

#define MAX_NAME_LEN 64
typedef struct {
    int id;
    char name[MAX_NAME_LEN];
} User;
#endif //RAMFORGE_USER_H
