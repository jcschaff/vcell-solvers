#!/usr/bin/perl
############################################################################
#
# Creates make.dep - a list of all dependencies
#
############################################################################

$obj_ext       = "o";
$linebreak     = "\\";
$dir_sep       = "/";
$nodepend      = 0;
$tolower       = 1; # lowercase all filenames
$include_path  = "";

#
# main - top level code
#

if ( @ARGV ) {				# parse command line args
    foreach $a ( @ARGV ) {
        if ( $a eq "-w" ) {
            $warnon = 1;
        }
        elsif ( ( $a eq "-?" ) || ( $a eq "-h" ) ) {
            print "usage: perl mkdep.pl includedirs...\"\n\n" .
              "\"includedirs\" is any number of include directories that are located\n".
              "outside of your project directory tree.\n\n".
              "example:\n   perl mkdep.pl s:\\ w:\\include\n";
            exit 1;
        }
        else {
            push @extradirs, $a;
            $include_path = $include_path . $a . ";";
        }
    }
    print "Includepath used: ".join(" ",@extradirs)."\n";
}

&make_files;
&make_dep;
    
exit 0;


#
# make_files: read "make.files", create "make.ofiles"
#

sub make_files {

    # create output file (now, to check for permissions)
    open OFILES, ">make.ofiles" or
        die("Can't create make.ofiles");

    # read file list
    open FILES,"<make.files" or 
        die "Can't open make.files";
    @lines = <FILES>;
    close FILES;

    # sort out header and source files
    @hdr = sort grep(/\.(h|hpp|icc|ia)$/i,@lines);
    @src = sort grep(/\.(c|cpp|cc|s)$/i,@lines);

    print STDERR scalar @lines ." files found (".scalar @src." sources and ".scalar @hdr." headers) in \"make.files\".\n";

    # scan specified include dirs
    for ( @extradirs ) {
        push @extrafiles, &find_files($_,"\.*",1);
    }
    # add external includefiles to file list
    push @lines, @extrafiles;

    print STDERR scalar @lines . " files found everywhere.\n";

    # create @inc and %full_path
    $last = "";
    for ( @lines ) {
        m/^(.*\/)([^\/]*)$/ || m/^()(.*)$/;
        $path = $1;
        $filepart = $2;
        if ( $path ne $last ) {
            # store dir path
            push (@inc, $path );
            $last = $path;
        }

        # remove any newlines at end of line
        $filepart =~ s/(.*)\n+$/$1/g;

        # store full path for all include files
        if ( !defined $full_path{ lc $filepart } ) {
            $full_path{lc $filepart} = $path.$filepart;
        }
    }

    $includes = join (" \\\n\t-I", @inc);
    $includes =~ s/\/ \\/ \\/g; # remove trailing slashes

    # save source list for use by make_depend
    $sourcelist = join(" ",@src);

    # create object list from source list
    $objects = &Objects( $sourcelist );

    # save object list for use by make_depend
    $objectlist = $objects;

    # make a "lints" list for linting
    $lints = $objects;
    $lints =~ s/\.${obj_ext}/\.lint/g;
    $lints =~ s/ / \\\n\t/g;

    # put newline+tab on all files (to make nice lists)
    $objects =~ s/ / \\\n\t/g;

    # print file
    print OFILES "# Do not edit this file! It is automatically generated. Changes will be lost.\n\n";
    print OFILES "OBJECTS = \\\n\t" . $objects . "\n\n";
    print OFILES "LINTS = \\\n\t" . $lints . "\n\n";
    if ( $includes ne "" ) {
        print OFILES "INCLUDEPATH = \\\n\t-I" . $includes . "\n";
    }

    close OFILES;
}



#
# Finds files.
#
# Examples:
#   find_files("/usr","\.cpp$",1)   - finds .cpp files in /usr and below
#   find_files("/tmp","^#",0)	    - finds #* files in /tmp
#

sub find_files {
    my($dir,$match,$descend) = @_;
    my($file,$p,@files);
    local(*D);
    $dir =~ s=\\=/=g;
    ($dir eq "") && ($dir = ".");

    if ( opendir(D,$dir) ) {
        if ( $dir eq "." ) {
            $dir = "";
        } else {
            ($dir =~ /\/$/) || ($dir .= "/");
        }
        foreach $file ( readdir(D) ) {
            
            next if ( $file  =~ /^\.\.?$/ );
            $p = $dir . $file;
#            ($file =~ /$match/i) && (push @files, ($tolower==0 ? $p : lc($p)));
            ($file =~ /$match/i) && (push @files, $p );
            if ( $descend && -d $p && ! -l $p ) {
                push @files, &find_files($p,$match,$descend);
            }
        }
        closedir(D);
    }
    return @files;
}


#
# strip_project_val(tag)
#
# Strips white space from project value strings.
#

sub strip_project_val {
    my($v) = @_;
    $v =~ s/^\s+//;				# trim white space
    $v =~ s/\s+$//;
    return $v;
}

#
# Objects(files)
#
# Replaces any extension with .o ($obj_ext).
#

sub Objects {
    local($_) = @_;
    my(@a);
    @a = split(/\s+/,$_);
    foreach ( @a ) {
        s-\.\w+$-.${obj_ext}-;
    }
    return join(" ",@a);
}


sub make_dep {

    $outfile = "make.dep";
    
    print STDERR "Parsing source files...\n";

    open(DEP,">" . fix_path($outfile)) or
        die ("Can't create \"$outfile\"");

    &BuildObj( $objectlist, $sourcelist );

    print DEP "# Do not edit this file! It is automatically generated. Changes will be lost.\n\n";
    print DEP $text;
    close DEP;
    print STDERR "\n";


    if ( $warnon && (keys %missing) ) {
        # print out missing files
        if ( !open MISSING, ">makemiss.txt" ) {
            print "Couldn't create \"makemiss.txt\": $!\n";
            print "Printing on screen.\n";
            *MISSING = *STDOUT;
        }
    
        print MISSING "Missing files: (Note: only the first miss for each file is logged)\n\n";
        printf MISSING "%-32s %s\n","<file>","<included from>";
        printf MISSING "%-32s %s\n","------","---------------";
        @missingfiles = sort @missingfiles;
        foreach ( @missingfiles ) {
            @a = split( ",", $_ );
            printf MISSING "%-32s %s\n",$a[0],$a[1];
        }
        close MISSING;

        print STDOUT "(missing files written to \"makemiss.txt\")\n";
    }
}


#
# BuildObj(objects,sources)
#
# Builds the object files.
#

sub BuildObj {
    my($obj,$src) = @_;
    my(@objv,$srcv,$i,$s,$o,$d,$c,$comp,$cimp);
    $text = "";
    @objv = split(/\s+/,$obj);
    @srcv = split(/\s+/,$src);
    $tot = $#objv;

    # fix dependpath
    if ( ! $depend_path_fixed ) {
        $depend_path_fixed = 1;
        $depend_path = $include_path;
        $count = 0;

        while ( $count < 100 ) {
            if ( $depend_path =~ s/(\$[\{\(]?\w+[\}\)]?)/035/ ) {
                $_ = $1;
                s/[\$\{\}\(\)]//g;
                $depend_path =~ s/035/$ENV{$_}/g;
            } else {
                $count = 100;
            }
        }
        @dep_path = &split_path($depend_path);
        unshift @dep_path, ""; # current dir first
    }

    %missing = ();
    # go through file list
    for $i ( 0..$#objv ) {
        $s = $srcv[$i];
        $o = $objv[$i];
        next if $s eq "";

        if ( $warnon ) {
            print STDERR "\n" . ($i+1) . "   missing files: ". scalar keys %missing;
        }
        else {
            # print STDERR "\n" . ($i+1);
        }

        @incfiles = ();
        $d = &make_depend(lc $s);

        for ( @incfiles ) {
            push @ifiles, $full_path{$n};
        }

        $text .= $o . ": ${linebreak}\n\t" . $s;

        @incpath = ();
        for ( @incfiles ) {
            if ( defined $full_path{$_} ) {
                push @incpath, $full_path{$_};
            }
        }
        @incpath = sort @incpath;
        for ( @incpath ) {
            $text .= " ${linebreak}\n\t" . $_;
        }
        $text .= "\n\n";

        # -----------------------------
#        print "\n". $text;
#        exit;
    }
    chop $text;
}


#
# build_dep() - Internal for make_depend()
#

sub build_dep {
    my($file) = @_;
    my(@i,$a,$n);
    $a = "";

    if ( !defined $depend_dict{$file} ) {
        return $a;
    }

    @i = split(/ /,$depend_dict{$file});
#    print "INC2: ($file) ".$depend_dict{$file}."\n";

    for $n ( @i ) {
#        if ( (!defined $dep_dict{$n}) && defined($full_path{$n}) ) {
        if ( !defined $dep_dict{$n} ) {
            $dep_dict{$n} = 1;
            push @incfiles, $n;
#            $a .= $full_path{$n} . " " . &build_dep($n);
            &build_dep($n);
        }
    }
#    print "INCFILES ($file): ".@incfiles."\n";
    return $a;
}


#
# make_depend(file)
#
# Returns a list of included files.
# Uses the global $depend_path variable.
#

sub make_depend {
    my($file) = @_;
    my($i,$count);
    if ( $nodepend ) {
        return "";
    }

    @cur_dep_path = @dep_path;
    if ( $file =~ /(.*[\/\\])/ ) {
        $dep_curdir = $1;
        splice( @cur_dep_path, 0, 0, $dep_curdir );
    } else {
        $dep_curdir = "";
    }
    $dep_file = $file;
    &canonical_dep($file);
    %dep_dict = ();


    $i = &build_dep($file);
#    chop $i;

    $i =~ s=/=$dir_sep=g unless $is_unix;
    $i =~ s=([a-zA-Z]):/=//$1/=g if (defined($gnuwin32) && $gnuwin32);
#    @l = sort split(/ /,$i);
    @l = split(/ /,$i);
    return join(" ${linebreak}\n\t", @l );
#    return $i; # all on one line!
}

#
# canonical_dep(file) - Internal for make_depend()
#
# Reads the file and all included files recursively.
# %depend_dict associates a file name to a list of included files.
#

sub canonical_dep {
    my($file) = @_;
    my(@inc,$i);
    push @sfile, $file;
    # -----------------------------
#    print "FILE: $file\n";
    @inc = &scan_dep($file);

    if ( @inc ) {
        $depend_dict{$file} = join(" ",@inc);

        # recursively scan all files not already scanned
        for $i ( @inc ) {
            if ( ! defined( $depend_dict{$i} ) ) {
                &canonical_dep($i);

                # still nothing defined?
                if ( ! defined( $depend_dict{$i} ) ) {
                    # insert dummy string, so we don't parse the file again
                    $depend_dict{$i} = "";
                }
#                print "CACHE: $i: $depend_dict{$i}\n";
            }
        }
    }
    pop @sfile, $file;
}

#
# scan_dep(file) - Internal for make_depend()
#
# Returns an array of included files.
#

sub scan_dep {
    my($file) = @_;
    my($dir,$path,$found,@allincs,@includes,%incs);
    $path = ($file eq $dep_file) ? $file : $dep_curdir . $file;
    @includes = ();

#    print STDERR "SCAN_DEP $file\n";

    # replace backslash with regular slash
    $file =~ s-\\-/-g;

    # look in the file list
    if ( !defined $full_path{$file} ) {
        # file not in list - some special case

        # handle explicit path'ed includes (such as #include "common/bsp821.h")
        if ( $file =~ /[\/\\]([^\\\/]+)$/ ) {
            $full_path{$file} = $full_path{$1};
        }
    }

    $open = open TMP,fix_path( $full_path{$file} );

    if ( $open ) {
        @source = <TMP>;

        # find all lines with include as first text in line (no comments allowed)
        @allincs = grep(/^\s*[\#\.]\s*include/,@source); 

#        print "SCAN_ALLINC: $#allincs include lines found\n";

        # iterate all include lines
        foreach ( @allincs ) {
            # parse out filename
            if ( !(/\s*[\#\.]\s*include\s+[<\"]([^>\"]*)[>\"]/) || defined($incs{$1}) ) {
                next;
            }
            push(@includes, ($tolower==0 ? $1 : lc($1)));

#           print "SCAN_INC: $1\n";

            $incs{$1} = "1";
        }

        # -----------------------------
#        print "INC: ".join( ",", @includes)."\n";
        close(TMP);
    }
    else {
        &add_missing( $file );
    }
    $/ = "\n";
    return @includes;
}


sub add_missing {
    my($file) = @_;
    @s = @sfile;
    pop @s;
    push @missingfiles, $file.",".join(" -> ",@s);
    $missing{$file} = 1;
}

#
# split_path(path)
#
# Splits a path containing : (Unix) or ; (MSDOS, NT etc.) separators.
# Returns an array.
#

sub split_path {
    my($p) = @_;
    return ""  if !defined($p);
    $p =~ s=:=;=g if $is_unix;
    $p =~ s=[/\\]+=/=g;
    $p =~ s=([^/:]);=$1/;=g;
    $p =~ s=([^:;/])$=$1/=;
    $p =~ s=/=$dir_sep=g unless $is_unix;
    return split(/;/,$p);
}

#
# fix_path(path)
#
# Converts all '\' to '/' if this really seems to be a Unix box.
#

sub fix_path {
    my($p) = @_;
#    if ( $really_unix ) {
#        $p =~ s-\\-/-g;
#    } else {
#        $p =~ s-/-\\-g;
        $p =~ s-\\-/-g;
#    }
    return $p;
}