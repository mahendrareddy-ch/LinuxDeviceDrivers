#include <stdio.h>
//#include <cstdio.h>

void main()
{
	char name[5];
	printf("Enter your desired string\n");
	scanf("%s",&name);
	char get_name[5];
	printf("Reading the string using gets function\n");
	//gets(get_name);

	printf("Printing the string using printf function: %s\n",name);
	printf("Printing the string using puts function\n");
	//puts(get_name);
}

