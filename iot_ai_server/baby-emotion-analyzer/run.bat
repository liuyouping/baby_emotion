@echo off
chcp 65001 >nul
cd /d "%~dp0"

echo =============================================
echo   婴幼儿人脸情绪分析  (MediaPipe + 规则引擎)
echo =============================================
echo.

rem 检查 python
where python >nul 2>nul
if errorlevel 1 (
    echo [错误] 未检测到 Python, 请先安装 Python 3.9+ 并加入 PATH.
    echo 下载: https://www.python.org/downloads/
    pause
    exit /b 1
)

rem 第一次运行自动安装依赖
if not exist ".deps\_installed" (
    echo 首次运行: 正在安装依赖 (mediapipe / opencv / numpy) ...
    python -m pip install -r requirements.txt -i https://pypi.tuna.tsinghua.edu.cn/simple --trusted-host pypi.tuna.tsinghua.edu.cn
    if errorlevel 1 (
        echo [警告] 清华镜像失败, 改用默认源重试...
        python -m pip install -r requirements.txt
    )
    if errorlevel 1 (
        echo [错误] 依赖安装失败, 请检查网络或手动执行:  pip install -r requirements.txt
        pause
        exit /b 1
    )
    mkdir ".deps" 2>nul
    echo done > ".deps\_installed"
    echo.
)

echo 选择运行模式:
echo   1) 批量分析   (自动扫描 imgs\baby 目录下所有图片)
echo   2) 指定单张图片
echo   3) 摄像头实时  (需要有摄像头)
echo.
set /p choice="请输入 1 / 2 / 3 (默认 1): "

if "%choice%"=="" set choice=1
if "%choice%"=="1" (
    echo.
    python baby_emotion.py --input imgs\baby --output output
) else if "%choice%"=="2" (
    set /p img_path="请输入图片路径: "
    echo.
    python baby_emotion.py --image "%img_path%"
) else if "%choice%"=="3" (
    echo 启动摄像头...按 q 退出, s 保存当前帧.
    python baby_emotion.py --camera 0
) else (
    echo 无效选择.
)

echo.
pause
