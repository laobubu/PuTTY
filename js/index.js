$.ajax('https://api.github.com/repos/laobubu/PuTTY/releases/latest')
    .then(function(r){
        var $download = $('#download')
        $('.version',  $download).text(r.name)
        $('.descript', $download).html(marked(r.body))
        $('.files ul', $download).append(
            r.assets.map(function(asset){
                $a = $('<a></a>')
                    .attr("href", asset.browser_download_url)
                    .text(asset.name)
                $li = $('<li></li>').append($a)
                return $li
            })
        )
    })