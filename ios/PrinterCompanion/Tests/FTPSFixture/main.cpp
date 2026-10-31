#include "PrinterFTPS.h"
#include <cstdio>
int main(int argc,char **argv) {
 if(argc!=5)return 2;
 char error[512]={};
 int code=printer_ftps_upload(argv[1],"127.0.0.1",argv[2],"synthetic-test-code","fixture.gcode.3mf",argv[3],error,sizeof(error));
 std::printf("%d %s\n",code,error);
 return code;
}
