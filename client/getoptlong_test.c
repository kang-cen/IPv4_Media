#include <stdio.h>
#include <getopt.h>

int main(int argc, char *argv[]) {
    struct option argarr[] = {
        {"port", 1, NULL, 'P'},
        {"mgroup", 1, NULL, 'M'},
        {"player",1,NULL,'p'},
        {"help", 0, NULL, 'H'},
        {NULL, 0, NULL, 0}
    };
    int count=0;
    int c, index;
    opterr = 1; // 启用错误信息
    while ((c = getopt_long(argc, argv, "P:M:p:H", argarr, &index)) != -1) {
        count++;
        printf("count: %d \n",count);
        switch (c) {
            case 'P':
                printf("Port: %s\n", optarg);
                break;
            case 'M':
                printf("Multicast group: %s\n", optarg);
                break;
            case 'p':
                printf("Other param: %s\n", optarg);
                break;
            case 'H':
                printf("Usage: %s [--port PORT] [--mgroup MGROUP] [-p PARAM] [-H]\n", argv[0]);
                break;
            case '?':
                fprintf(stderr, "Error: Invalid option or missing argument\n");
                return 1;
            default:
                fprintf(stderr, "Unexpected error\n");
                return 1;
        }
    }

    // 处理非选项参数（如果有）
    for (; optind < argc; optind++) {
        printf("Non-option argument: %s\n", argv[optind]);
    }

    return 0;
}


