## 1、项目位置：

> 对应基于OpenHarmony5.0的WS63E开发环境
```
applications/sample/wifi-iot/app/ws63e
```


## 2、项目列表： 

### 项目名称：稚声慧眸，camera_code

```
applications/sample/wifi-iot/app/ws63e/camera_code
```

在项目内的BUILD.gn文件中：
```
applications/sample/wifi-iot/app/ws63e/camera_code/BUILD.gn
```
如下修改：
```
static_library("zhwl_app")
```
 
 
## 3、编译配置

``` 
applications/sample/wifi-iot/app/BUILD.gn
```
修改BUILD.gn文件：
```
lite_component("app") {
  features = [ 
    ......
    #"hi3861/ams_demo:zhwl_app",
    #"hi3861/audio_demo:zhwl_app", 
    #"ws63e/lcd_wave_client:zhwl_app",
    #"ws63e/lcd_wave_server:zhwl_app",

    "ws63e/camera_code:zhwl_app",
  ]
}
```

## 4、组件配置：

```
device/soc/hisilicon/ws63v100/sdkv102/build/config/target_config/ws63/config.py
```

在以下位置添加新组件：
```
'ram_component': [
    ……,
    #添加新组件
    "zhwl_app"
    ]
```

```
device/soc/hisilicon/ws63v100/sdkv102/libs_url/ws63/cmake/ohos.cmake
```

在以下位置添加新组件：
```
elseif(${TARGET_COMMAND} MATCHES "ws63-liteos-app")
    set(COMPONENT_LIST
        ……,
        # 添加新组件 
        "zhwl_app"
    )
endif()
```

## 5、编译指令：

在cmd等命令行中登录ubuntu指定文件夹
```
ssh harmony@192.168.118.133
```
```
cd near/near_link_sdk/
```

```
rm -rf out/* && hb set -p nearlink_dk_3863 && hb build -f
```

编译生成的bin文件：
``` 
out/nearlink_dk_3863/nearlink_dk_3863/ws63-liteos-app/ws63-liteos-app_all.fwpkg
```

使用BurnTool（绿色）进行烧录，重启开发板后在sscomm工具中观察输出信息。