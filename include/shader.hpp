#pragma once

#include <glad/glad.h>
#include <string>
#include <sstream>
#include <iostream>
#include <fstream>
#include <filesystem>

class Shader{
public:
    Shader(const char* vertexPath, const char* fragmentPath){
        std::ifstream ifs_vertex;
        std::ifstream ifs_fragment;

        std::string vertexStr, fragmentStr;
        //ifs_vertex.exceptions(std::ifstream::failbit | std::ifstream::badbit);
        //ifs_fragment.exceptions(std::ifstream::failbit | std::ifstream::badbit);
        //if (!ifs_vertex.is_open()) {
        //    std::cerr << "Failed to open vertex shader: "
        //        << vertexPath << '\n';
        //    return;
        //}
        //if (!ifs_fragment.is_open()) {
        //    std::cerr << "Failed to open fragment shader: "
        //        << fragmentPath << '\n';
        //    return;
        //}
        try{
            ifs_vertex.open(vertexPath,std::ios::in);
            std::cerr << "vertex shader opened" << std::endl;
            ifs_fragment.open(fragmentPath,std::ios::in);
            std::cerr << "fragment shader opened" << std::endl;
            std::stringstream ss_vertex;
            std::stringstream ss_fragment;
            ss_vertex << ifs_vertex.rdbuf();
            ss_fragment << ifs_fragment.rdbuf();
            ifs_vertex.close();
            ifs_fragment.close();
            vertexStr = ss_vertex.str();
            fragmentStr = ss_fragment.str();
        }
        catch(const std::exception& e){
            std::cerr << "Shader file error: " << e.what() << '\n';
            throw;
        }

        std::cerr << "Vertex shader length: "
            << vertexStr.size() << '\n';

        std::cerr << "Fragment shader length: "
            << fragmentStr.size() << '\n';

        const char* vertexSource = vertexStr.c_str();
        const char* fragmentSource = fragmentStr.c_str();

        int success;
        char infoLog[512];

        unsigned int vertex, fragment;
        vertex = glCreateShader(GL_VERTEX_SHADER);
        fragment = glCreateShader(GL_FRAGMENT_SHADER);

        glShaderSource(vertex,1,&vertexSource,nullptr);
        glCompileShader(vertex);

        glGetShaderiv(vertex,GL_COMPILE_STATUS,&success);
        if(!success){
            glGetShaderInfoLog(vertex,512,nullptr,infoLog);
            std::cerr << __FILE__ << " " << __LINE__ << " vertex shader compile failed : " << infoLog << std::endl;
            return;
        }

        glShaderSource(fragment,1,&fragmentSource,nullptr);
        glCompileShader(fragment);

        glGetShaderiv(fragment,GL_COMPILE_STATUS,&success);
        if(!success){
             std::cerr << __FILE__ << " " << __LINE__ << " fragment shader compile failed : " << infoLog << std::endl;
            return;
        }

        ID = glCreateProgram();
        glAttachShader(ID,vertex);
        glAttachShader(ID,fragment);
        glLinkProgram(ID);
        glGetProgramiv(ID,GL_LINK_STATUS,&success);
        if(!success){
            glGetProgramInfoLog(ID,512,nullptr,infoLog);
            std::cerr << __FILE__ << " " << __LINE__ <<  " Shader Program Link failed : " << infoLog << std::endl;
            return;
        }
        std::cerr << "Shader link success, ID = "
            << ID << std::endl;

        glDeleteShader(vertex);
        glDeleteShader(fragment);
    }

    void use(){
        glUseProgram(ID);
    }

    unsigned int GetShaderPragram(){
        return ID;
    }

    ~Shader(){
        glDeleteProgram(ID);
    }

    
    unsigned int ID = 0;
};