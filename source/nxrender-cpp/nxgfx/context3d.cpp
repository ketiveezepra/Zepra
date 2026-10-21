// Copyright (c) 2025 KetiveeAI. All rights reserved.
// Licensed under KPL-2.0. See LICENSE file for details.
/**
 * @file context3d.cpp
 * @brief 3D Graphics implementation with OpenGL
 */

#include "nxgfx/context3d.h"
#include "nxgfx/gl_includes.h"
#ifdef __APPLE__
#include <OpenGL/glu.h>
#else
#include <GL/glu.h>
#endif
#include <cmath>

namespace NXRender {

// ==========================================================================
// Mesh Generation
// ==========================================================================

Mesh Mesh::cube(float size) {
    Mesh m;
    float h = size / 2;
    
    // 8 vertices, 6 faces, 12 triangles
    // Front face
    m.vertices.push_back({{-h, -h, h}, {0, 0, 1}});
    m.vertices.push_back({{h, -h, h}, {0, 0, 1}});
    m.vertices.push_back({{h, h, h}, {0, 0, 1}});
    m.vertices.push_back({{-h, h, h}, {0, 0, 1}});
    
    // Back face
    m.vertices.push_back({{h, -h, -h}, {0, 0, -1}});
    m.vertices.push_back({{-h, -h, -h}, {0, 0, -1}});
    m.vertices.push_back({{-h, h, -h}, {0, 0, -1}});
    m.vertices.push_back({{h, h, -h}, {0, 0, -1}});
    
    // Top face
    m.vertices.push_back({{-h, h, h}, {0, 1, 0}});
    m.vertices.push_back({{h, h, h}, {0, 1, 0}});
    m.vertices.push_back({{h, h, -h}, {0, 1, 0}});
    m.vertices.push_back({{-h, h, -h}, {0, 1, 0}});
    
    // Bottom face
    m.vertices.push_back({{-h, -h, -h}, {0, -1, 0}});
    m.vertices.push_back({{h, -h, -h}, {0, -1, 0}});
    m.vertices.push_back({{h, -h, h}, {0, -1, 0}});
    m.vertices.push_back({{-h, -h, h}, {0, -1, 0}});
    
    // Right face
    m.vertices.push_back({{h, -h, h}, {1, 0, 0}});
    m.vertices.push_back({{h, -h, -h}, {1, 0, 0}});
    m.vertices.push_back({{h, h, -h}, {1, 0, 0}});
    m.vertices.push_back({{h, h, h}, {1, 0, 0}});
    
    // Left face
    m.vertices.push_back({{-h, -h, -h}, {-1, 0, 0}});
    m.vertices.push_back({{-h, -h, h}, {-1, 0, 0}});
    m.vertices.push_back({{-h, h, h}, {-1, 0, 0}});
    m.vertices.push_back({{-h, h, -h}, {-1, 0, 0}});
    
    // Indices for 6 faces
    for (int i = 0; i < 6; i++) {
        uint32_t base = i * 4;
        m.indices.push_back(base); m.indices.push_back(base + 1); m.indices.push_back(base + 2);
        m.indices.push_back(base); m.indices.push_back(base + 2); m.indices.push_back(base + 3);
    }
    
    return m;
}

Mesh Mesh::sphere(float radius, int segments) {
    Mesh m;
    
    for (int y = 0; y <= segments; y++) {
        float phi = 3.14159f * y / segments;
        for (int x = 0; x <= segments; x++) {
            float theta = 2.0f * 3.14159f * x / segments;
            
            Vec3 pos(
                radius * std::sin(phi) * std::cos(theta),
                radius * std::cos(phi),
                radius * std::sin(phi) * std::sin(theta)
            );
            Vec3 normal = pos.normalized();
            
            Vertex3D v;
            v.position = pos;
            v.normal = normal;
            v.u = static_cast<float>(x) / segments;
            v.v = static_cast<float>(y) / segments;
            m.vertices.push_back(v);
        }
    }
    
    // Indices
    for (int y = 0; y < segments; y++) {
        for (int x = 0; x < segments; x++) {
            uint32_t i0 = y * (segments + 1) + x;
            uint32_t i1 = i0 + 1;
            uint32_t i2 = i0 + segments + 1;
            uint32_t i3 = i2 + 1;
            
            m.indices.push_back(i0); m.indices.push_back(i2); m.indices.push_back(i1);
            m.indices.push_back(i1); m.indices.push_back(i2); m.indices.push_back(i3);
        }
    }
    
    return m;
}

Mesh Mesh::plane(float width, float height) {
    Mesh m;
    float hw = width / 2, hh = height / 2;
    
    m.vertices.push_back({{-hw, 0, -hh}, {0, 1, 0}, Color::white(), 0, 0});
    m.vertices.push_back({{hw, 0, -hh}, {0, 1, 0}, Color::white(), 1, 0});
    m.vertices.push_back({{hw, 0, hh}, {0, 1, 0}, Color::white(), 1, 1});
    m.vertices.push_back({{-hw, 0, hh}, {0, 1, 0}, Color::white(), 0, 1});
    
    m.indices = {0, 1, 2, 0, 2, 3};
    return m;
}

Mesh Mesh::cylinder(float radius, float height, int segments) {
    Mesh m;
    float hh = height / 2;
    
    // Side vertices
    for (int i = 0; i <= segments; i++) {
        float theta = 2.0f * 3.14159f * i / segments;
        float x = radius * std::cos(theta);
        float z = radius * std::sin(theta);
        Vec3 normal(std::cos(theta), 0, std::sin(theta));
        
        m.vertices.push_back({{x, -hh, z}, normal});
        m.vertices.push_back({{x, hh, z}, normal});
    }
    
    // Side indices
    for (int i = 0; i < segments; i++) {
        uint32_t base = i * 2;
        m.indices.push_back(base); m.indices.push_back(base + 1); m.indices.push_back(base + 3);
        m.indices.push_back(base); m.indices.push_back(base + 3); m.indices.push_back(base + 2);
    }
    
    return m;
}

// ==========================================================================
// GpuContext3D
// ==========================================================================

GpuContext3D::GpuContext3D() {}
GpuContext3D::~GpuContext3D() {}

void GpuContext3D::begin3D(const Camera& camera) {
    camera_ = camera;
    
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    
    // Set up projection
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(camera_.fov * 180.0f / 3.14159f, camera_.aspectRatio, 
                   camera_.nearPlane, camera_.farPlane);
    
    // Set up view
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(camera_.position.x, camera_.position.y, camera_.position.z,
              camera_.target.x, camera_.target.y, camera_.target.z,
              camera_.up.x, camera_.up.y, camera_.up.z);
    
    // Lighting
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    
    GLfloat ambient[] = {ambientLight_.r / 255.f, ambientLight_.g / 255.f, 
                         ambientLight_.b / 255.f, 1.0f};
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
    
    GLfloat lightPos[] = {lightDir_.x, lightDir_.y, lightDir_.z, 0.0f};
    GLfloat lightDiffuse[] = {lightColor_.r / 255.f, lightColor_.g / 255.f, 
                              lightColor_.b / 255.f, 1.0f};
    glLightfv(GL_LIGHT0, GL_POSITION, lightPos);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, lightDiffuse);
}

void GpuContext3D::end3D() {
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
}

void GpuContext3D::setAmbientLight(const Color& color) {
    ambientLight_ = color;
}

void GpuContext3D::setDirectionalLight(const Vec3& direction, const Color& color) {
    lightDir_ = direction.normalized();
    lightColor_ = color;
}

void GpuContext3D::drawMesh(const Mesh& mesh, const Mat4& transform) {
    glPushMatrix();
    glMultMatrixf(transform.m);
    
    if (wireframe_) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }
    
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    
    glBegin(GL_TRIANGLES);
    for (uint32_t idx : mesh.indices) {
        const Vertex3D& v = mesh.vertices[idx];
        glColor4ub(v.color.r, v.color.g, v.color.b, v.color.a);
        glNormal3f(v.normal.x, v.normal.y, v.normal.z);
        glVertex3f(v.position.x, v.position.y, v.position.z);
    }
    glEnd();
    
    if (wireframe_) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    
    glDisable(GL_COLOR_MATERIAL);
    glPopMatrix();
}

void GpuContext3D::drawCube(const Vec3& center, float size, const Color& color) {
    Mesh cube = Mesh::cube(size);
    for (auto& v : cube.vertices) {
        v.color = color;
    }
    drawMesh(cube, Mat4::translate(center.x, center.y, center.z));
}

void GpuContext3D::drawSphere(const Vec3& center, float radius, const Color& color) {
    Mesh sphere = Mesh::sphere(radius, 16);
    for (auto& v : sphere.vertices) {
        v.color = color;
    }
    drawMesh(sphere, Mat4::translate(center.x, center.y, center.z));
}

void GpuContext3D::drawLine3D(const Vec3& start, const Vec3& end, const Color& color) {
    glDisable(GL_LIGHTING);
    glColor4ub(color.r, color.g, color.b, color.a);
    glBegin(GL_LINES);
    glVertex3f(start.x, start.y, start.z);
    glVertex3f(end.x, end.y, end.z);
    glEnd();
    glEnable(GL_LIGHTING);
}

void GpuContext3D::drawGrid(float size, int divisions) {
    glDisable(GL_LIGHTING);
    glColor4ub(100, 100, 100, 255);
    
    float half = size / 2;
    float step = size / divisions;
    
    glBegin(GL_LINES);
    for (int i = 0; i <= divisions; i++) {
        float pos = -half + i * step;
        // X-axis lines (parallel to Z)
        glVertex3f(pos, 0, -half);
        glVertex3f(pos, 0, half);
        // Z-axis lines (parallel to X)
        glVertex3f(-half, 0, pos);
        glVertex3f(half, 0, pos);
    }
    glEnd();
    
    glEnable(GL_LIGHTING);
}

} // namespace NXRender
