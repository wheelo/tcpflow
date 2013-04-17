/*
 * This file is part of tcpflow by Simson Garfinkel <simsong@acm.org>.
 * Originally by Jeremy Elson <jelson@circlemud.org>.
 *
 * This source code is under the GNU Public License (GPL) version 3.
 * See COPYING for details.
 *
 */

#define __MAIN_C__

#include "tcpflow.h"
#include "tcpip.h"
#include "tcpdemux.h"
#include "bulk_extractor_i.h"
#include "iptree.h"
#include <string>
#include <vector>

#include <sys/types.h>
#include <dirent.h>

/* bring in inet_ntop if it is not present */
#define ETH_ALEN 6
#ifndef HAVE_INET_NTOP
#include "inet_ntop.c"
#endif

int iphtest=0;
size_t iphtrim=0;

scanner_info::config_t be_config; // system configuration

typedef struct {
    const char *name;
    const char *dvalue;
    const char *help;
} default_t;

default_t defaults[] = {
    {"tdelta","0","Time delta in seconds"},
    {0,0,0}
};

#ifdef HAVE_NETINET_IP_H
#include <netinet/ip.h>
#endif

const char *progname = 0;		// name of the program
int debug = DEFAULT_DEBUG_LEVEL;	// global variable, not clear why

/* semaphore prevents multiple copies from outputing on top of each other */
#ifdef HAVE_PTHREAD
#include <semaphore.h>
sem_t *semlock = 0;
#endif

#define DEFAULT_REPORT_FILENAME "report.xml"

/****************************************************************
 *** SCANNER PLUG-IN SYSTEM 
 ****************************************************************/

scanner_t *scanners_builtin[] = {
    scan_md5,
    scan_http,
    scan_netviz,
    scan_tcpdemux,
    0};

bool opt_no_promisc = false;		// true if we should not use promiscious mode

/****************************************************************
 *** USAGE
 ****************************************************************/

static int usage_count = 0;
static void usage()
{
    switch(++usage_count){
    case 1:
        std::cout << PACKAGE_NAME << " version " << PACKAGE_VERSION << "\n\n";
        std::cout << "usage: " << progname << " [-aBcCDhpsv] [-b max_bytes] [-d debug_level] [-f max_fds]\n";
        std::cout << "      [-i iface] [-L semlock] [-r file] [-R file] [-w file] [-o outdir] [-X xmlfile]\n";
        std::cout << "      [-m min_bytes] [-F[ct]] [expression]\n\n";
        std::cout << "   -a: do ALL post-processing.\n";
        std::cout << "   -b max_bytes: max number of bytes per flow to save\n";
        std::cout << "   -d debug_level: debug level; default is " << DEFAULT_DEBUG_LEVEL << "\n";
        std::cout << "   -f: maximum number of file descriptors to use\n";
        std::cout << "   -h: print this help message (-hh for more help)\n";
        std::cout << "   -i: network interface on which to listen\n";
        std::cout << "   -J: output each flow in alternating colors (note change!)\n";
        std::cout << "   -l: treat non-flag arguments as input files rather than a pcap expression\n";
        std::cout << "   -L  semlock - specifies that writes are locked using a named semaphore\n";
        std::cout << "   -p: don't use promiscuous mode\n";
        std::cout << "   -q: quiet mode - do not print warnings\n";
        std::cout << "   -r file: read packets from tcpdump pcap file (may be repeated)\n";
        std::cout << "   -R file: read packets from tcpdump pcap file TO FINISH CONNECTIONS\n";
        std::cout << "   -w file: write packets not processed to file\n";
        std::cout << "   -S name=value  Set a configuration parameter (-hh for info)\n";
        std::cout << "   -v: verbose operation equivalent to -d 10\n";
        std::cout << "   -V: print version number and exit\n";
        std::cout << "   -o  outdir   : specify output directory (default '.')\n";
        std::cout << "   -X  filename : DFXML output to filename\n";
        std::cout << "   -m  bytes    : specifies skip that starts a new stream (default "
                  << (unsigned)tcpdemux::options::MAX_SEEK << ").\n";
        std::cout << "   -F{p} : filename prefix/suffix (-hh for options)\n";
        std::cout << "   -T{t} : filename template (-hh for options; default "
                  << flow::filename_template << ")\n";
        std::cout << "   -Z: do not decompress gzip-compressed HTTP transactions\n";
        info_scanners(false,scanners_builtin,'e','x');

        std::cout << "Console output options:\n";
        std::cout << "   -B: binary output, even with -c or -C (normally -c or -C turn it off)\n";
        std::cout << "   -c: console print only (don't create files)\n";
        std::cout << "   -C: console print only, but without the display of source/dest header\n";
        std::cout << "   -s: strip non-printable characters (change to '.')\n";
        std::cout << "   -D: output in hex (useful to combine with -c or -C)\n";
        std::cout << "\n";
#ifndef HAVE_LIBCAIRO
	std::cout << "Rendering not available because Cairo was not installed.\n\n";
#endif
        std::cout << "expression: tcpdump-like filtering expression\n";
        std::cout << "\nSee the man page for additional information.\n\n";
        break;
    case 2:
        std::cout << "Filename Prefixes:\n";
        std::cout << "   -Fc : append the connection counter to ALL filenames\n";
        std::cout << "   -Ft : prepend the time_t timestamp to ALL filenames\n";
        std::cout << "   -FT : prepend the ISO8601 timestamp to ALL filenames\n";
        std::cout << "   -FX : Do not output any files (other than report files)\n";
        std::cout << "   -FM : Calculate the MD5 for every flow (stores in DFXML)\n";
        std::cout << "   -Fk : Bin output in 1K directories\n";
        std::cout << "   -Fm : Bin output in 1M directories (2 levels)\n";
        std::cout << "   -Fg : Bin output in 1G directories (3 levels)\n";
        flow::usage();
        std::cout << "-S name=value options:\n";
        for(int i=0;defaults[i].name;i++){
            std::stringstream ss;
            ss << defaults[i].name << "=" << defaults[i].dvalue;
            printf("   %-20s %s\n",ss.str().c_str(),defaults[i].help);
        }
        std::cout << "\n";
        std::cout << "DEBUG Levels (specify with -dNN):\n";
        std::cout << "get_max_fds() = " << tcpdemux::getInstance()->get_max_fds() << "\n";
        std::cout << "NUM_RESERVED_FDS = " << NUM_RESERVED_FDS << "\n";
        break;
    }
}

/**
 * Create the dfxml output
 */

static void dfxml_create(class xml &xreport,const std::string &command_line)
{
    xreport.push("dfxml","xmloutputversion='1.0'");
    xreport.push("metadata",
		 "\n  xmlns='http://afflib.org/tcpflow/' "
		 "\n  xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' "
		 "\n  xmlns:dc='http://purl.org/dc/elements/1.1/'" );
    xreport.xmlout("dc:type","Feature Extraction","",false);
    xreport.pop();
    xreport.add_DFXML_creator(PACKAGE_NAME,PACKAGE_VERSION,"",command_line);
    xreport.push("configuration");
    xreport.pop();			// configuration
}


/* String replace. Perhaps not the most efficient, but it works */
void replace(std::string &str,const std::string &from,const std::string &to)
{
    if(from.size()==0) return;
    bool changed = false;

    std::stringstream ss;
    for(unsigned int i=0;i<str.size();){
	if(str.substr(i,from.size())==from){
	    ss << to;
	    i+=from.size();
	    changed = true;
	} else {
	    ss << str.at(i);
	    i++;
	}
    }
    if(changed) str = ss.str();			// copy over original
}

/* These must be global variables so they are available in the signal handler */
feature_recorder_set *the_fs = 0;
xml *xreport = 0;
void terminate(int sig)
{
    DEBUG(1) ("terminating");
    phase_shutdown(*the_fs,*xreport);	// give plugins a chance to do a clean shutdown
    exit(0); /* libpcap uses onexit to clean up */
}

#ifdef HAVE_FORK
// transparent decompression for process_infile
class inflater {
public:
    inflater(regex_t regex_, std::string invoc_format_) :
        regex(regex_), invoc_format(invoc_format_) {}
    // is this inflater appropriate for a given file?
    bool appropriate(const std::string &file_path) const
    {
        return regexec(&regex, file_path.c_str(), (size_t) 0, NULL, 0) == 0;
    }
    // invoke the inflater in a shell, and return the file descriptor to read the inflated file from
    int invoke(const std::string &file_path) const
    {
        std::string invocation = ssprintf(invoc_format.c_str(), file_path.c_str());
        int pipe_fds[2];
        if(!system(NULL)) {
            std::cerr << "no shell available to decompress '" << file_path << "'" << std::endl;
            return -1;
        }
        if(pipe(pipe_fds)) {
            std::cerr << "failed to create pipe to decompress '" << file_path << "'" << std::endl;
            return -1;
        }

        pid_t child_pid;
        child_pid = fork();
        if(child_pid == -1) {
            std::cerr << "failed to fork child to decompress '" << file_path << "'" << std::endl;
            return -1;
        }
        if(child_pid == 0) {
            // decompressor
            close(pipe_fds[0]);
            dup2(pipe_fds[1], 1);
            if(system(invocation.c_str())) {
                std::cerr << "decompressor reported error inflating '" << file_path << "'" << std::endl;
                exit(1);
            }
            exit(0);
        }
        close(pipe_fds[1]);
        return pipe_fds[0];
    }
    regex_t regex;
    std::string invoc_format;
};

static std::vector<inflater> build_inflaters()
{
    std::vector<inflater> output;
    regex_t re;

    // gzip
    regcomp(&re, "\\.gz$", REG_EXTENDED);
    output.push_back(inflater(re, "gunzip -c '%s'"));
    // zip
    regcomp(&re, "\\.zip$", REG_EXTENDED);
    output.push_back(inflater(re, "unzip -p '%s'"));
    // bz2
    regcomp(&re, "\\.bz2$", REG_EXTENDED);
    output.push_back(inflater(re, "bunzip2 -c '%s'"));
    // xz
    regcomp(&re, "\\.xz$", REG_EXTENDED);
    output.push_back(inflater(re, "unxz -c '%s'"));
    // lzma
    regcomp(&re, "\\.lzma$", REG_EXTENDED);
    output.push_back(inflater(re, "unlzma -c '%s'"));

    return output;
}

static std::vector<inflater> inflaters = build_inflaters();
#define HAVE_INFLATER
#endif

/*
 * process an input file or device
 * May be repeated.
 * If start is false, do not initiate new connections
 */
static void process_infile(const std::string &expression,const char *device,const std::string &infile)
{
    char error[PCAP_ERRBUF_SIZE];
    pcap_t *pd=0;
    int dlt=0;
    pcap_handler handler;

    if (infile!=""){
        std::string file_path = infile;
        // decompress input if necessary
#ifdef HAVE_INFLATER
        for(std::vector<inflater>::const_iterator it = inflaters.begin(); it != inflaters.end(); it++) {
            if(it->appropriate(infile)) {
                int fd = it->invoke(infile);
                file_path = ssprintf("/dev/fd/%d", fd);
                if(fd < 0) {
                    std::cerr << "decompression of '" << infile << "' failed" << std::endl;
                    exit(1);
                }
                if(access(file_path.c_str(), R_OK)) {
                    std::cerr << "decompression of '" << infile << "' is not available on this system" << std::endl;
                    exit(1);
                }
                break;
            }
        }
#endif
	if ((pd = pcap_open_offline(file_path.c_str(), error)) == NULL){	/* open the capture file */
	    die("%s", error);
	}
	dlt = pcap_datalink(pd);	/* get the handler for this kind of packets */
	handler = find_handler(dlt, infile.c_str());
    } else {
	/* if the user didn't specify a device, try to find a reasonable one */
	if (device == NULL){
	    if ((device = pcap_lookupdev(error)) == NULL){
		die("%s", error);
	    }
	}

	/* make sure we can open the device */
	if ((pd = pcap_open_live(device, SNAPLEN, !opt_no_promisc, 1000, error)) == NULL){
	    die("%s", error);
	}
#if defined(HAVE_SETUID) && defined(HAVE_GETUID)
	/* drop root privileges - we don't need them any more */
	if(setuid(getuid())){
	    perror("setuid");
	}
#endif
	/* get the handler for this kind of packets */
	dlt = pcap_datalink(pd);
	handler = find_handler(dlt, device);
    }

    /* If DLT_NULL is "broken", giving *any* expression to the pcap
     * library when we are using a device of type DLT_NULL causes no
     * packets to be delivered.  In this case, we use no expression, and
     * print a warning message if there is a user-specified expression
     */
#ifdef DLT_NULL_BROKEN
    if (dlt == DLT_NULL && expression != ""){
	DEBUG(1)("warning: DLT_NULL (loopback device) is broken on your system;");
	DEBUG(1)("         filtering does not work.  Recording *all* packets.");
    }
#endif /* DLT_NULL_BROKEN */

    DEBUG(20) ("filter expression: '%s'",expression.c_str());

    /* install the filter expression in libpcap */
    struct bpf_program	fcode;
    if (pcap_compile(pd, &fcode, expression.c_str(), 1, 0) < 0){
	die("%s", pcap_geterr(pd));
    }

    if (pcap_setfilter(pd, &fcode) < 0){
	die("%s", pcap_geterr(pd));
    }

    /* initialize our flow state structures */

    /* set up signal handlers for graceful exit (pcap uses onexit to put
     * interface back into non-promiscuous mode
     */
    portable_signal(SIGTERM, terminate);
    portable_signal(SIGINT, terminate);
#ifdef SIGHUP
    portable_signal(SIGHUP, terminate);
#endif

    /* start listening or reading from the input file */
    if (infile == "") DEBUG(1) ("listening on %s", device);
    if (pcap_loop(pd, -1, handler, (u_char *)tcpdemux::getInstance()) < 0){
	
	die("%s: %s", infile.c_str(),pcap_geterr(pd));
    }
}




int main(int argc, char *argv[])
{
    bool didhelp = false;
#ifdef BROKEN
    std::cerr << "WARNING: YOU ARE USING AN EXPERIMENTAL VERSION OF TCPFLOW \n";
    std::cerr << "THAT DOES NOT WORK PROPERLY. PLEASE USE A RELEASE DOWNLOADED\n";
    std::cerr << "FROM http://digitalcorpora.org/downloads/tcpflow\n";
    std::cerr << "\n";
#endif

    bool force_binary_output = false;
    const char *device = "<default>";
    const char *lockname = 0;
    int need_usage = 0;
    std::string reportfilename;
    std::vector<std::string> Rfiles;	// files for finishing
    std::vector<std::string> rfiles;	// files to read
    tcpdemux &demux = *tcpdemux::getInstance();			// the demux object we will be using.
    std::string command_line = xml::make_command_line(argc,argv);
    std::string opt_unk_packets;
    bool opt_quiet = false;

    /* Set up debug system */
    progname = argv[0];
    init_debug(progname,1);

    /* Make sure that the system was compiled properly */
    if(sizeof(struct be13::ip4)!=20 || sizeof(struct be13::tcphdr)!=20){
	fprintf(stderr,"COMPILE ERROR.\n");
	fprintf(stderr,"  sizeof(struct ip)=%d; should be 20.\n", (int)sizeof(struct be13::ip4));
	fprintf(stderr,"  sizeof(struct tcphdr)=%d; should be 20.\n", (int)sizeof(struct be13::tcphdr));
	fprintf(stderr,"CANNOT CONTINUE\n");
	exit(1);
    }

    bool trailing_input_list = false;
    int arg;
    while ((arg = getopt(argc, argv, "aA:Bb:cCd:De:E:F:f:Hhi:JlL:m:o:pqR:r:S:sT:Vvw:x:X:Z")) != EOF) {
	switch (arg) {
	case 'a':
	    demux.opt.post_processing = true;
	    demux.opt.opt_md5 = true;
	    scanners_enable_all();
	    break;

	case 'A': 
	    fprintf(stderr,"-AH has been deprecated. Just use -a\n");
	    need_usage=true;
	    break;

	case 'b':
	    demux.opt.max_bytes_per_flow = atoi(optarg);
	    if(debug > 1) {
		std::cout << "capturing max of " << demux.opt.max_bytes_per_flow << " bytes per flow." << std::endl;
	    }
	    break;
	case 'B':
            force_binary_output = true;
	    demux.opt.output_strip_nonprint  = false;	DEBUG(10) ("converting non-printable characters to '.'");
	    break;
	case 'C':
	    demux.opt.console_output  = true;	DEBUG(10) ("printing packets to console only");
	    demux.opt.suppress_header = 1;	DEBUG(10) ("packet header dump suppressed");
	    break;
	case 'c':
	    demux.opt.console_output = true;	DEBUG(10) ("printing packets to console only");
	    break;
	case 'd':
	    if ((debug = atoi(optarg)) < 0) {
		debug = DEFAULT_DEBUG_LEVEL;
		DEBUG(1) ("warning: -d flag with 0 debug level '%s'", optarg);
	    }
	    break;
        case 'D':
            demux.opt.output_hex = true;DEBUG(10) ("Console output in hex");
	    demux.opt.output_strip_nonprint = false;	DEBUG(10) ("Will not convert non-printablesto '.'");
            break;
        case 'e': scanners_enable(optarg); break;
	case 'F':
	    for(const char *cc=optarg;*cc;cc++){
		switch(*cc){
		case 'c': replace(flow::filename_template,"%c","%C"); break;
                case 'k': flow::filename_template = "%K/" + flow::filename_template; break;
                case 'm': flow::filename_template = "%M000-%M999/%M%K/" + flow::filename_template; break;
                case 'g': flow::filename_template = "%G000000-%G999999/%G%M000-%G%M999/%G%M%K/" + flow::filename_template; break;
		case 't': flow::filename_template = "%tT" + flow::filename_template; break;
		case 'T': flow::filename_template = "%T"  + flow::filename_template; break;
		case 'X': demux.opt.store_output = false;break;
		case 'M': demux.opt.opt_md5 = true;break;
		default:
		    fprintf(stderr,"-F invalid format specification '%c'\n",*cc);
		    need_usage = true;
		}
	    }
	    break;
	case 'f':
        {
            int mnew = atoi(optarg);
            DEBUG(1)("changing max_fds from %d to %d",demux.max_fds,mnew);
            demux.max_fds = mnew;
	    break;
        }
	case 'i': device = optarg; break;
	case 'J':
	    demux.opt.use_color  = 1;
	    DEBUG(10) ("using colors");
	    break;
        case 'l': trailing_input_list = true; break;
	case 'L': lockname = optarg; break;
	case 'm':
	    demux.opt.max_seek = atoi(optarg);
	    DEBUG(10) ("max_seek set to %d",demux.opt.max_seek); break;
	case 'o':
            demux.outdir = optarg;
            flow::outdir = optarg;
            break;
	case 'p': opt_no_promisc = true; DEBUG(10) ("NOT turning on promiscuous mode"); break;
        case 'q': opt_quiet = true; break;
	case 'R': Rfiles.push_back(optarg); break;
	case 'r': rfiles.push_back(optarg); break;
        case 'S':
	    {
		std::vector<std::string> params = split(optarg,'=');
		if(params.size()!=2){
		    std::cerr << "Invalid paramter: " << optarg << "\n";
		    exit(1);
		}
		be_config[params[0]] = params[1];
		continue;
	    }

	case 's':
            demux.opt.output_strip_nonprint = 1; DEBUG(10) ("converting non-printable characters to '.'");
            break;
	case 'T': flow::filename_template = optarg;break;
	case 'V': std::cout << PACKAGE_NAME << " " << PACKAGE_VERSION << "\n"; exit (1);
	case 'v': debug = 10; break;
        case 'w': opt_unk_packets = optarg;break;
	case 'x': scanners_disable(optarg);break;
	case 'X': reportfilename = optarg;break;
	case 'Z': demux.opt.gzip_decompress = 0; break;
	case 'H':
            info_scanners(true,scanners_builtin,'e','x');
            didhelp = true;
            break;
	case 'h': case '?':
            usage();
            didhelp = true;
            break;
	default:
	    DEBUG(1) ("error: unrecognized switch '%c'", arg);
	    need_usage = 1;
	    break;
	}
    }

    if(didhelp) exit(0);
    if(demux.opt.post_processing && !demux.opt.store_output){
        std::cerr << "ERROR: post_processing currently requires storing output.\n";
        exit(1);
    }

    argc -= optind;
    argv += optind;

    /* Load all the scanners and enable the ones we care about */
    if(demux.opt.opt_md5) scanners_enable("md5");
    load_scanners(scanners_builtin,be_config);
    scanners_process_commands();

    /* If there is no report filename, call it report.xml in the output directory */
    if( reportfilename.size()==0 ){
	reportfilename = demux.outdir + "/" + DEFAULT_REPORT_FILENAME;
    }

    /* print help and exit if there was an error in the arguments */
    if (need_usage) {
	usage();
	exit(1);
    }

    /* remaining arguments are either an input list (-l flag) or a pcap expression (default) */
    std::string expression = "";
    if(trailing_input_list) {
        for(int ii = 0; ii < argc; ii++) {
            rfiles.push_back(argv[ii]);
        }
    }
    else {
        /* get the user's expression out of remainder of the arg... */
        for(int i=0;i<argc;i++){
            if(expression.size()>0) expression+=" ";
            expression += argv[i];
        }
    }

    /* More option processing */

    /* was a semaphore provided for the lock? */
    if(lockname){
#if defined(HAVE_SEMAPHORE_H) && defined(HAVE_PTHREAD)
	semlock = sem_open(lockname,O_CREAT,0777,1); // get the semaphore
#else
	fprintf(stderr,"%s: attempt to create lock pthreads not present\n",argv[0]);
	exit(1);
#endif	
    }

    if(force_binary_output) demux.opt.output_strip_nonprint = false;
    /* make sure outdir is a directory. If it isn't, try to make it.*/
    struct stat stbuf;
    if(stat(demux.outdir.c_str(),&stbuf)==0){
	if(!S_ISDIR(stbuf.st_mode)){
	    std::cerr << "outdir is not a directory: " << demux.outdir << "\n";
	    exit(1);
	}
    } else {
	if(MKDIR(demux.outdir.c_str(),0777)){
	    std::cerr << "cannot create " << demux.outdir << ": " << strerror(errno) << "\n";
	    exit(1);
	}
    }

    std::string input_fname;
    if(rfiles.size() > 0) {
        input_fname = rfiles.at(0);
        if(rfiles.size() > 1) {
            input_fname += ssprintf(" + %d more", rfiles.size() - 1);
        }
    }

    /* report file specified? */
    if(reportfilename.size()>0){
	xreport = new xml(reportfilename,false);
	dfxml_create(*xreport,command_line);
	demux.xreport = xreport;
    }
    if(opt_unk_packets.size()>0){
        if(input_fname.size()==0){
            std::cerr << "currently the -w option requires the -r option\n";
            exit(1);
        }
        if(access(input_fname.c_str(),R_OK)) die("cannot read: %s: %s",input_fname.c_str(),strerror(errno));
        demux.save_unk_packets(opt_unk_packets,input_fname);
    }

    /* Debug prefix set? */
    if(be_config.find("debug-prefix") != be_config.end()){
        init_debug(be_config["debug-prefix"].c_str(),0);
    }

    argc -= optind;
    argv += optind;

    DEBUG(10) ("%s version %s ", PACKAGE_NAME, PACKAGE_VERSION);

    feature_file_names_t feature_file_names;
    enable_feature_recorders(feature_file_names);
    feature_recorder_set fs(feature_file_names,input_fname.size()>0 ? input_fname : device,demux.outdir,false);
    the_fs   = &fs;
    demux.fs = &fs;

    datalink_tdelta = atoi(be_config["tdelta"].c_str()); // specify the time delta
    iphtest         = atoi(be_config["iphtest"].c_str()); // get iphtesting
    iphtrim     = atoi(be_config["iphtrim"].c_str()); // get iphtrim

    if(demux.xreport) demux.xreport->xmlout("tdelta",datalink_tdelta);

    /* Process r files and R files */
    if(rfiles.size()==0 && Rfiles.size()==0){
	/* live capture */
#if defined(HAVE_SETUID) && defined(HAVE_GETUID)
        /* Since we don't need network access, drop root privileges */
	if(setuid(getuid())){
	    perror("setuid");
	}
#endif
	demux.start_new_connections = true;
        process_infile(expression,device,"");
        input_fname = device;
    }
    else {
	/* first pick up the new connections with -r */
	demux.start_new_connections = true;
	for(std::vector<std::string>::const_iterator it=rfiles.begin();it!=rfiles.end();it++){
	    process_infile(expression,device,*it);
	}
	/* now pick up the outstanding connection with -R, but don't start new connections */
	demux.start_new_connections = false;
	for(std::vector<std::string>::const_iterator it=Rfiles.begin();it!=Rfiles.end();it++){
	    process_infile(expression,device,*it);
	}
    }

    /* -1 causes pcap_loop to loop forever, but it finished when the input file is exhausted. */

    DEBUG(2)("Open FDs at end of processing:      %d",(int)demux.open_flows.size());
    DEBUG(2)("demux.max_open_flows:               %d",(int)demux.max_open_flows);
    DEBUG(2)("Flow map size at end of processing: %d",(int)demux.flow_map.size());
    DEBUG(2)("Flows seen:                         %d",(int)demux.flow_counter);

    demux.close_all_fd();
    phase_shutdown(fs,*xreport);
    
    /*
     * Note: funny formats below are a result of mingw problems with PRId64.
     */
    const std::string total_flow_processed("Total flows processed: %"PRId64);
    const std::string total_packets_processed("Total packets processed: %"PRId64);
    
    DEBUG(2)(total_flow_processed.c_str(),demux.flow_counter);
    DEBUG(2)(total_packets_processed.c_str(),demux.packet_counter);

    if(xreport){
	demux.remove_all_flows();	// empty the map to capture the state
	xreport->add_rusage();
	xreport->pop();                 // bulk_extractor
	xreport->close();
	delete xreport;                 
    }

    if(demux.flow_counter > tcpdemux::WARN_TOO_MANY_FILES){
        if(!opt_quiet){
            /* Start counting how many files we have in the output directory.
             * If we find more than 10,000, print the warning, and keep counting...
             */
            uint64_t filecount=0;
            DIR *dirp = opendir(demux.outdir.c_str());
            if(dirp){
                struct dirent *dp=0;
                while((dp=readdir(dirp))!=NULL){
                    filecount++;
                    if(filecount==10000){
                        std::cerr << "*** tcpflow WARNING:\n";
                        std::cerr << "*** Modern operating systems do not perform well \n";
                        std::cerr << "*** with more than 10,000 entries in a directory.\n";
                        std::cerr << "***\n";
                    }
                }
                closedir(dirp);
            }
            if(filecount>=10000){
                std::cerr << "*** tcpflow created " << filecount << " files in output directory " << demux.outdir << "\n";
                std::cerr << "***\n";
                std::cerr << "*** Next time, specify command-line options: -Fk , -Fm , or -Fg \n";
                std::cerr << "*** This will automatically bin output into subdirectories.\n";
                std::cerr << "*** type 'tcpflow -hhh' for more information.\n";
            }
        }
    }

    exit(0);                            // return(0) causes crash on Windows
}
