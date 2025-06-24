
# Test Wiredancer DMA

Quick and dirty dma test that pushes dummy data through fpga wd pipeline and
checks to see if it writes back to host memory over pcim. 

## what it does

- wd_init_pci
- wd_ed25519_verify_init_req
- wd_ed25519_verify_req
- wd_snp_cntrs
- flush cache line
- read dma buffer (must be non-zero)
- read vled to get addr written

## Install and run

- git clone https://github.com/abklabs/svmkit-examples-wiredancer
- cd svmkit-examples-wiredancer
- set up aws creds (requires access to f2)
- pulumi install && pulumi up -y
- cd ..

- git clone http://github.com/monological/aws-fpga
- cd aws-fpga
- source sdk_setup.sh
- cd ..

- git clone http://github.com/monological/firedancer
- cd firedancer
- git checkout milestone-1.4-demo-abk-mcache
- cd ..

- git clone http://github.com/monological/test_wd_dma
- cd test_wd_dma
- make clean
- make
- echo 8 | sudo tee /proc/sys/vm/nr_hugepages # enable hugepages
- ./test_dma


