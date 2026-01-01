BudEngine-repo/
├── vcpkg/                 # Git submodule
│   ├── scripts/
│   └── ports/
├── vcpkg.json            # Project Dependencies
├── CMakeLists.txt        # Construct
├── src/                  
└── README.md             


# 项目名称

## 构建说明
### 依赖管理
### 快速开始
### 详细配置
### 项目结构
### 常见问题

# Naming Style

- Types: PascalCase (MyClass, WidgetMgr)  
- Functions/Variables: snake_case (update_state, current_value)  
- Private Members: snake_case with trailing underscore (value_)  
- Namespaces: lowercase  
- Constants: kPascalCaseor snake_case  




# Note

- Do git submodule update --init --recursive after cloning this repository.  
- When you use Visual Studio to work, please add following configs into lauch.vs.json file.  
-- "currentDir": "${workspaceRoot}"  
-- "env": { "PATH": "${env.PATH};${workspaceRoot}/vcpkg/installed/x64-windows/bin" }  