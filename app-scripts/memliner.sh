#!/bin/bash

app=$1
ratio=$2

# Configure spark master IP here:
# spark_master=spark://IP:7277

# You can get jars and data files from https://drive.google.com/file/d/10G0BA3ThPRtaoPCIGp-ikXpQ8YdiUusn/view?usp=sharing

spark_path="${HOME}/spark-3.0.0-preview2-bin-hadoop3.2"

echo "true" > /sys/kernel/mm/swap/vma_ra_enabled
echo 16 > /sys/kernel/mm/swap/readahead_win

if [[ ${ratio} == "25" ]]
then
    echo 9g > /sys/fs/cgroup/memory/memctl/memory.limit_in_bytes
elif [[ ${ratio} == "13" ]]
then
    echo 5g > /sys/fs/cgroup/memory/memctl/memory.limit_in_bytes
fi

if [[ ${app} == "lr" ]]
then
    if [[ ${ratio} == "25" ]]
    then
        (time -p ${spark_path}/bin/spark-submit --name "My app" --master $spark_master --conf spark.eventLog.enabled=false --class SparkLR data/jars/lr.jar data/data/lr.dat 10)
    elif [[ ${ratio} == "13" ]]
    then
        (time -p ${spark_path}/bin/spark-submit --name "My app" --master $spark_master --conf spark.eventLog.enabled=false --class SparkLR data/jars/lr.jar data/data/lr.dat 10)
    fi
fi
if [[ ${app} == "km" ]]
then
    if [[ ${ratio} == "25" ]]
    then
        (time -p ${spark_path}/bin/spark-submit --name "My app" --master $spark_master --conf spark.eventLog.enabled=false --class JavaKMeansExample data/jars/km.jar data/data/km.dat 4 10)
    elif [[ ${ratio} == "13" ]]
    then
        (time -p ${spark_path}/bin/spark-submit --name "My app" --master $spark_master --conf spark.eventLog.enabled=false --class JavaKMeansExample data/jars/km.jar data/data/km.dat 4 10)
    fi
fi
if [[ ${app} == "tc" ]]
then
    if [[ ${ratio} == "100" ]] || [[ ${ratio} == "25" ]]
    then
        (time -p ${spark_path}/bin/spark-submit --name "My app" --master $spark_master --conf spark.eventLog.enabled=false --class org.apache.spark.basic.SparkTC data/jars/tc.jar 32 3)
    elif [[ ${ratio} == "13" ]]
    then
        (time -p ${spark_path}/bin/spark-submit --name "My app" --master $spark_master --conf spark.eventLog.enabled=false --class org.apache.spark.basic.SparkTC data/jars/tc.jar 32 3)
    fi
fi