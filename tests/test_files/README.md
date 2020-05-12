Klaus2m5 files should patched in order to generate the right binary images:

org zero_page

should be changed to

org 0
ds zero_page

before assembling
