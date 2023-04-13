import os, re

path = 'C:\\temp\linuxcnc-2.9\\linuxcnc-2.9\\docs\\src\\hal' # only needed if run from different directory
path = "."

DOC_SRCDIR = "C:\\temp\linuxcnc-2.9\\linuxcnc-2.9\\docs\\src\\hal"

findall = False
findall = True

files = []
# r=root, d=directories, f = files
if findall is True:
    for r, d, f in os.walk(path):
        for file in f:
            if '.adoc' in file:
                # files.append(os.path.join(r, file))
                # files.append(os.path.relpath(os.path.join(r, file), DOC_SRCDIR))
                files.append(os.path.relpath(os.path.join(r, file)))
else:
    files = [
            "C:\\temp\\linuxcnc-2.9\\linuxcnc-2.9\\docs\\src\\test.adoc"
            ]


anchors = {}

## 1. create dict holding the file path with the anchors
for file in files:
    with open(os.path.join(path, file), 'r', encoding='utf-8') as fp:
        lines = fp.readlines()
        for line in lines:
            anchor = re.findall("\[\[[^\[\]]*\]\]", line)
            if anchor:
                if len(anchor) > 1:
                    print("    [Error] More than one anchor in one line in '{}'".format(file))
                else:
                    anchor = anchor[0].strip("[]")
                    anchors[anchor] = file
                    # print(file, anchor)

def findAnchorInFile(filelist, anchor):
    for line in filelist:
        if line.find("[["+anchor+"]]") >= 0:
            return True
    return False


## 2. add relative pathes to cross-references but not for references within the same file
# example: <<config/stepconf.adoc#cha:stepconf-wizard,Stepper Configuration Wizard>>

lines = []
for file in files:
    file_changed = False
    with open(os.path.join(path, file), 'r', encoding='utf-8') as fp:
        refs_intern = 0
        refs_extern = 0
        lines = fp.readlines()
        for i, line in enumerate(lines):
            for anchor in anchors:
                # Simple search (faster than regex search)
                link = line.find("<<"+anchor)
                if link >= 0:
                    refs_extern += 1
                    # Check if anchor is in same file - we dont't have to do anything then
                    if not findAnchorInFile(lines, anchor):
                        complete_link = re.findall("<<"+anchor+".*>>", line)
                        if(len(complete_link) > 0):
                            # print("found cross-reference for", anchor, "in", file)
                            # print(complete_link[0])
                            lines[i] = line.replace(complete_link[0], "<<../"+anchors[anchor]+"#"+complete_link[0].strip("<>")+">>")
                            file_changed = True
                            # print("new line:", lines[i])
                        else:
                            print("    [Error] Cross-reference in file '{}' includes a linke break. Fix manually!".format(file))
                    else:
                        refs_intern += 1
                        # print("found reference inside file for", anchor, "in", file)
                        pass

    if file_changed:
        # Summary
        print("{:50} internal refs: {}, external refs: {} ".format(file, refs_intern, refs_extern))
        with open(os.path.join(path, file), 'w', encoding='utf-8') as fp:
            fp.writelines(lines)
        

# TODO: fix for more than one reference in one line