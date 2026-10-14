/* SPDX-License-Identifier: GPL-2.0 */
/* Kernel-only CPU-clock samples around WORKLOAD_BEGIN/CPU output markers.
 * Counts are sampling evidence, not function durations or exact CPU accounting.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

struct symbol { uint64_t address; char *name; };
static struct symbol *symbols;
static size_t symbol_count;
static unsigned long counts[6], lost;
static struct perf_event_mmap_page *metadata;
static unsigned char *ring;
static size_t ring_size;
static uint64_t tail;
static int event = -1;
static const char *categories[] = {
 "preparation", "fl2000_usb", "usb_xhci", "fl2000_control", "other_kernel", "unknown"
};
static int compare(const void *a, const void *b)
{
 const struct symbol *x=a,*y=b;
 return (x->address>y->address)-(x->address<y->address);
}
static void read_symbols(void)
{
 FILE *file=fopen("/proc/kallsyms","r");
 if(!file)return;
 char line[512],name[256],type;unsigned long long address;
 size_t capacity=0;
 while(fgets(line,sizeof(line),file)) {
  if(sscanf(line,"%llx %c %255s",&address,&type,name)!=3||!address)continue;
  if(symbol_count==capacity){capacity=capacity?capacity*2:8192;symbols=realloc(symbols,capacity*sizeof(*symbols));if(!symbols)exit(1);}
  symbols[symbol_count++]=(struct symbol){address,strdup(name)};
 }
 fclose(file);qsort(symbols,symbol_count,sizeof(*symbols),compare);
}
static const char *resolve(uint64_t address)
{
 size_t lo=0,hi=symbol_count;
 if(address>=(uint64_t)-4095)return "?";
 while(lo<hi){size_t middle=lo+(hi-lo)/2;if(symbols[middle].address<=address)lo=middle+1;else hi=middle;}
 return lo?symbols[lo-1].name:"?";
}
static void copy_ring(void *dst,uint64_t position,size_t size)
{
 size_t offset=position%ring_size,first=ring_size-offset;
 if(first>size)first=size;
 memcpy(dst,ring+offset,first);memcpy((char *)dst+first,ring,size-first);
}
static void sample(const unsigned char *data,size_t size)
{
 if(size<32)return;
 uint64_t ip,nr;memcpy(&ip,data+8,8);memcpy(&nr,data+24,8);
 if(nr>(size-32)/8)return;
 int category=5;
 for(uint64_t i=0;i<=nr;i++) {
  uint64_t address=ip;if(i)memcpy(&address,data+32+(i-1)*8,8);
  const char *name=resolve(address);int candidate=5;
  if(strstr(name,"fl2000_convert")||strstr(name,"fl2000_reorder")||strstr(name,"fl2000_frame_convert")||strstr(name,"fl2000_ssse3")||strstr(name,"fl2000_bmi2"))candidate=0;
  else if(strstr(name,"fl2000_submit_transfer")||strstr(name,"fl2000_data_completion"))candidate=1;
  else if(strstr(name,"xhci")||strstr(name,"usb_hcd")||strstr(name,"usb_submit_urb"))candidate=2;
  else if(strstr(name,"fl2000")||strstr(name,"it66121"))candidate=3;
  else if(strcmp(name,"?"))candidate=4;
  if(candidate<category)category=candidate;
 }
 counts[category]++;
}
static void drain(void)
{
 if(!metadata)return;
 uint64_t head=__atomic_load_n(&metadata->data_head,__ATOMIC_ACQUIRE);
 while(tail<head) {
  struct perf_event_header header;unsigned char data[4096];
  copy_ring(&header,tail,sizeof(header));
  if(header.size<sizeof(header)||header.size>sizeof(data)||header.size>head-tail){lost++;tail=head;break;}
  copy_ring(data,tail,header.size);
  if(header.type==PERF_RECORD_SAMPLE)sample(data,header.size);
  else if(header.type==PERF_RECORD_LOST && header.size>=24){uint64_t value;memcpy(&value,data+16,8);lost+=value;}
  tail+=header.size;
 }
 __atomic_store_n(&metadata->data_tail,tail,__ATOMIC_RELEASE);
}
static void setup(void)
{
 struct perf_event_attr attr={.size=sizeof(attr),.type=PERF_TYPE_SOFTWARE,
  .config=PERF_COUNT_SW_CPU_CLOCK,.sample_freq=1999,.freq=1,
  .sample_type=PERF_SAMPLE_IP|PERF_SAMPLE_TID|PERF_SAMPLE_CALLCHAIN,
  .disabled=1,.exclude_user=1,.exclude_idle=1,.exclude_hv=1,
  .sample_max_stack=32,.wakeup_events=1};
 event=syscall(__NR_perf_event_open,&attr,-1,0,-1,PERF_FLAG_FD_CLOEXEC);
 if(event<0){printf("KERNEL_SAMPLE_UNAVAILABLE errno=%d (%s)\n",errno,strerror(errno));return;}
 size_t page=sysconf(_SC_PAGESIZE);ring_size=256*page;
 metadata=mmap(NULL,page+ring_size,PROT_READ|PROT_WRITE,MAP_SHARED,event,0);
 if(metadata==MAP_FAILED){metadata=NULL;close(event);event=-1;perror("sample mmap");return;}
 ring=(unsigned char *)metadata+page;read_symbols();
 printf("KERNEL_SAMPLE_READY frequency=1999 symbols=%zu\n",symbol_count);
}
static void line(const char *value)
{
 static char format[32],phase[32];
 if(!strncmp(value,"WORKLOAD_BEGIN ",15)) {
  if(event>=0){ioctl(event,PERF_EVENT_IOC_DISABLE,0);drain();memset(counts,0,sizeof(counts));lost=0;ioctl(event,PERF_EVENT_IOC_RESET,0);ioctl(event,PERF_EVENT_IOC_ENABLE,0);}
  sscanf(value+15,"%31s %31s",format,phase);
 }
 puts(value);
 if(!strncmp(value,"CPU ",4) && event>=0) {
  ioctl(event,PERF_EVENT_IOC_DISABLE,0);drain();
  printf("KERNEL_SAMPLES %s %s",format,phase);
  for(size_t i=0;i<sizeof(counts)/sizeof(counts[0]);i++)printf(" %s=%lu",categories[i],counts[i]);
  printf(" lost=%lu\n",lost);
 }
}
int main(int argc,char **argv)
{
 if(argc<2){fprintf(stderr,"usage: kernel-sample COMMAND [ARGS...]\n");return 2;}
 setbuf(stdout,NULL);setup();
 int pipefd[2];if(pipe(pipefd)){perror("pipe");return 1;}
 pid_t child=fork();if(child<0){perror("fork");return 1;}
 if(!child){close(pipefd[0]);dup2(pipefd[1],STDOUT_FILENO);dup2(pipefd[1],STDERR_FILENO);close(pipefd[1]);execvp(argv[1],argv+1);perror("exec");_exit(127);}
 close(pipefd[1]);char buffer[8192];size_t length=0;int done=0;
 while(!done) {
  struct pollfd fd={.fd=pipefd[0],.events=POLLIN};int result=poll(&fd,1,100);
  if(result<0 && errno!=EINTR){perror("poll");kill(child,SIGTERM);break;}
  drain();
  if(result>0 && (fd.revents&(POLLIN|POLLHUP))) {
   ssize_t size=read(pipefd[0],buffer+length,sizeof(buffer)-length-1);
   if(size<=0){done=1;continue;}length+=size;buffer[length]=0;
   char *start=buffer,*end;
   while((end=strchr(start,'\n'))){*end=0;line(start);start=end+1;}
   length-=start-buffer;memmove(buffer,start,length);
   if(length==sizeof(buffer)-1){buffer[length]=0;line(buffer);length=0;}
  }
 }
 close(pipefd[0]);if(event>=0)close(event);
 int status;if(waitpid(child,&status,0)<0)return 1;
 return WIFEXITED(status)?WEXITSTATUS(status):1;
}
