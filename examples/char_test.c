#include <stdio.h>

int main() {
    char arr[] = {'H', 'e', 'l', 'l', 'o'};
    // Calculate the total number of elements in the array
    int size = sizeof(arr) / sizeof(arr[0]); 

    for (int i = 0; i < size; i++) {
        printf("%c\n", arr[i]); // Prints each character on a new line
    }

    return 0;
}
