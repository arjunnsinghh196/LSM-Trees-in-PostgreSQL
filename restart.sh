sudo killall -9 make
sudo killall -9 clang-17
sudo killall -9 llvm-lto
sudo killall -9 postgres
make clean
rm -f *.bc
make with_llvm=no
sudo make install with_llvm=no
sudo make install
sudo service postgresql restart
sudo -u postgres psql