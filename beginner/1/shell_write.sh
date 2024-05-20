#!/bin/bash
FILENAME='temp.sh'
TEXTFILE='tempfile.txt'
echo '#!/bin/bash
STUDENT_ID="22222123456"
if [ "$#" -ne 2 ]; then
    echo "$0 用法错误，请输入文件名与标志位"
    exit 1
fi
filename="$1"
rw="$2"
if [ "$rw" = "w" ]; then
    last="${STUDENT_ID: -3}"
    echo "$last MYFILE" >"$filename"
    echo "写入成功到 $filename"
    echo "写入成功，内容：$last MYFILE"
elif [ "$rw" = "r" ]; then
    file=$(cat "$filename")
    if [ $? -ne 0 ]; then
        echo "读取失败！"
    else
        echo "读取成功，内容："
        echo "$file"
    fi
else
    echo "用法错误，标志位只能是r或w"
fi
' >"$FILENAME"
echo "写入脚本成功到 $FILENAME"

chmod +x "$FILENAME"
./temp.sh "$TEXTFILE" w
