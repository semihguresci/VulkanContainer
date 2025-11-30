#pragma once

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>


#include <MaterialXCore/Document.h>
#include <MaterialXFormat/XmlIo.h>

#include <string>

namespace utility::materialx {

class SlangMaterialXBridge {
public:
    SlangMaterialXBridge();

    MaterialX::DocumentPtr loadDocument(const std::string& filename);

    glm::vec4 extractBaseColor(const MaterialX::DocumentPtr& document) const;

private:
};

} // namespace utility::materialx
