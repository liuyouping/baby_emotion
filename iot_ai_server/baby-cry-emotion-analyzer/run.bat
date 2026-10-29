@echo off
chcp 65001 >nul
cd /d "%~dp0"

echo =============================================
echo   婴幼儿哭声情绪分析  (规则引擎 / 无模型)
echo =============================================
echo.

where python >nul 2>nul
if errorlevel 1 (
    echo [错误] 未检测到 Python, 请先安装 Python 3.9+
    pause
    exit /b 1
)

if not exist ".deps\_installed" (
    echo 首次运行: 正在安装依赖 (librosa / soundfile / scipy / numpy) ...
    python -m pip install -r requirements.txt -i https://pypi.tuna.tsinghua.edu.cn/simple --trusted-host pypi.tuna.tsinghua.edu.cn
    if errorlevel 1 (
        echo [警告] 清华镜像失败, 改用默认源...
        python -m pip install -r requirements.txt
    )
    if errorlevel 1 (
        echo [错误] 依赖安装失败
        pause
        exit /b 1
    )
    mkdir ".deps" 2>nul
    echo done > ".deps\_installed"
    echo.
)

echo 选择运行模式:
echo   1) 批量分析   (扫描 cries\ 目录)
echo   2) 指定单段音频
echo   3) 麦克风录音 5 秒后分析
echo.
set /p choice="请输入 1 / 2 / 3 (默认 1): "

if "%choice%"=="" set choice=1
if "%choice%"=="1" (
    python baby_cry_emotion.py --batch cries
) else if "%choice%"=="2" (
    set /p audio="请输入音频路径: "
    python baby_cry_emotion.py "%audio%"
) else if "%choice%"=="3" (
    python baby_cry_emotion.py --mic
) else (
    echo 无效选择
)

echo.
pause
