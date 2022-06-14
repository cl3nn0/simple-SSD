# Operating Systems Capstone 2022 Final Project

| GitHub account name | Student ID | Name   |
| ------------------- | ---------- | ------ |
| cl3nn0              | 310551034  | 張逸軍  |

## Install Library

```bash
apt-cache search fuse
apt-get update
apt-get -y install fuse3 libfuse3-dev pkg-config
```

## Run
```bash
mkdir /tmp/ssd /tmp/ssd_fuse
./make_ssd

# in terminal A
./ssd_fuse -d /tmp/ssd

# in terminal B
sh test.sh test1
sh test.sh test2
```
